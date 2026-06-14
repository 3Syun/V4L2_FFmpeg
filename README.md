# V4L2_FFmpeg

编译：
g++ viode.cc -o test     $(pkg-config --cflags --libs libavcodec libavformat libavutil)     -lpthread


ffmpeg常见命令：

1、查看视频详细信息：
ffmpeg -i video.mp4 -f null -   -f为指定输出设备 null表示空设备
ffprobe -i test.mp4   查看流信息

2、转换封装格式
ffmpeg -i input.mkv -c copy output.mp4
    -c copy 表示视频和音频都不重新编码，只是换个容器，几秒就能完成。
    如果源编码不符合目标容器规范（比如想把 H.265 放进 AVI），就会失败，此时需要重新编码

3、提取视频里的音频
ffmpeg -i video.mp4 -vn -c:a libmp3lame -q:a 2 output.mp3
    -vn 不要视频
    -c:a libmp3lame  用MP3编码器（需 FFmpeg 编译时支持）-》FFmpeg 在编译时加上 --enable-libmp3lame 这个配置选项
    -q:a 2：VBR 质量，0-9 数字越小质量越好（2 相当于 ~190kbps）
ffmpeg -i video.mp4 -vn -c:a aac -b:a 192k output.aac   提取ACC

4、截取视频片段
ffmpeg -ss 00:01:30 -i input.mp4 -to 00:02:00 -c copy cut.mp4
    -ss 00:01:30：开始时间（1 分 30 秒）
    -to 00:02:00：结束时间（剪切 30 秒片段）
    使用了 -c copy，速度极快，但切割点可能不是精确帧（关键帧切割），如果需要精确到帧，建议重新编码（去掉 -c copy 并指定编码参数），这时切割精度高但速度慢。
为什么不精确？——关键帧（I 帧）的限制
    视频压缩通常会把帧分成 I 帧（关键帧，完整图像）、P 帧和 B 帧（向前/后预测的差异帧）。一个 GOP（图像组）通常只有一个 I 帧，后面跟一堆 P/B 帧，这些帧依赖于 I 帧才能解码。
    当你用 -c copy 从某个时间点切割时，切割的起点和终点必须落在 I 帧上，否则解码器无法从那个地方独立解码。FFmpeg 会自动将切割点移动到离你指定时间最近的一个 I 帧，这就导致了实际切割出的片段比你预期的多几秒或少几秒。
需要“精确到帧”怎么办？
    就需要重新编码，也就是去掉 -c copy 并指定编码器参数
    ffmpeg -ss 00:01:30 -i input.mp4 -to 00:02:00 -c:v libx264 -c:a aac cut.mp4
    重新编码时，解码器会把所有帧解出来，切割以解码后的每一帧为准，因此可以做到帧级精确，但是会很慢且画面质量会略有损失；

5、视频压缩（减小体积，同时保持较好画质）
ffmpeg -i input.mp4 -c:v libx264 -preset slow -crf 23 -c:a aac -b:a 128k -movflags +faststart compressed.mp4    使用H.264 + CRF 控制
    -movflags +faststart：让 MP4 支持网页快速播放（moov atom 前置）
    CRF 调高（如 28）体积更小但画质下降；调低（如 18）体积增大但画质接近无损

6、改变分辨率
ffmpeg -i input.mp4 -vf "scale=1280:720" -c:v libx264 -crf 23 -preset medium -c:a copy output_720p.mp4
    -vf "scale=宽:高" 是视频滤镜。如果想保持宽高比，可将其中一个值设为 -1，如 scale=1280:-1

7、合并多个视频（要求编码、分辨率等一致）
先创建一个文件列表 list.txt：
text
file 'part1.mp4'
file 'part2.mp4'
file 'part3.mp4'
然后执行：
ffmpeg -f concat -safe 0 -i list.txt -c copy merged.mp4
如果文件编码不同，需要先转码为相同参数，否则合并可能出错。

8、添加简单水印（图片叠加）
ffmpeg -i video.mp4 -i logo.png -filter_complex "overlay=10:10" output.mp4
    overlay=10:10 表示水印离左边距 10 像素，上边距 10 像素。可以用 overlay=W-w-10:10 表示右上角。

9、给视频静音或替换音频
去除音频轨道：
ffmpeg -i input.mp4 -an -c:v copy silent.mp4
把新音频替换进去（原有视频流复制）：
ffmpeg -i video.mp4 -i new_audio.aac -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 -shortest output.mp4
     -map 用于选择流，-shortest 以较短的时长为准

--------------------------
viode.cc大概流程：
摄像头硬件
   ↓ (驱动填充)
V4L2 内核缓冲区
   ↓ mmap 映射到用户空间
采集线程 (cvideo)
   ↓ 拷贝帧到环形队列 + 携带时间戳
帧队列 (frame_queue)
   ↓ 条件变量通知
编码线程 (encoder_thread)
   ├── 从队列取出帧
   ├── 填充 AVFrame，计算相对 PTS
   ├── 送入 H.264 编码器 (avcodec_send_frame / avcodec_receive_packet)
   ├── 得到 AVPacket (压缩数据)
   ├── 写入 H.264 裸流文件 (fwrite)
   ├── 时间基转换后写入 MP4 容器 (av_interleaved_write_frame)
   └── 循环，直到退出

---具体流程如下：---
一、采集阶段（V4L2 单平面 MMAP 模式）

1、打开设备：open("/dev/video0", O_RDWR)，设置格式：
    类型  V4L2_BUF_TYPE_VIDEO_CAPTURE
    分辨率 640×480，
    像素格式 V4L2_PIX_FMT_NV12
    field 设为 V4L2_FIELD_ANY，其余由驱动填充
2、申请缓冲区：VIDIOC_REQBUFS，数量 4，
    模式 V4L2_MEMORY_MMAP
3、查询并映射：对每个缓冲区：
    VIDIOC_QUERYBUF 获取偏移/长度
    mmap 映射到用户空间，记录到 usebuf[i].start
    入队：VIDIOC_QBUF 将所有缓冲区放入采集队列
    开启视频流：VIDIOC_STREAMON
4、采集循环：
    VIDIOC_DQBUF 取出一个填满数据的缓冲区（阻塞或可被信号中断）
    获取 buf.timestamp（struct timeval）、buf.sequence、buf.bytesused
    将帧数据拷贝到自定义队列（queue_put），携带绝对时间戳（timeval→微秒整数）
    VIDIOC_QBUF 归还缓冲区
5、退出清理：
    VIDIOC_STREAMOFF、munmap、close
    唤醒编码线程（queue_wakeup_all）
二、线程间传递（环形队列 + 条件变量）
1、数据结构：环形数组 nodes[QUEUE_SIZE]，每个节点包含拷贝的 NV12 数据、字节数、微秒级 PTS、sequence 等。
2、互斥锁：保护队列所有操作。
3、两个条件变量：
    not_empty：队列空时编码线程等待；采集线程放入数据后 signal。
    not_full：队列满时采集线程等待；编码线程取出数据后 signal。
4、退出机制：信号处理函数设置 quit_flag，并通过 broadcast 唤醒所有阻塞线程，循环内检查 quit_flag 后退出。
三、编码阶段（FFmpeg libavcodec）
1、初始化编码器：
    avcodec_find_encoder(AV_CODEC_ID_H264) 找到软件 H.264 编码器
    avcodec_alloc_context3 分配编码上下文
2、设置：
    分辨率、
    像素格式 AV_PIX_FMT_NV12、
    编码器时间基 {1,1000000}（微秒基，与 PTS 单位一致）、
    码率 1 Mbps，
    GOP 大小 30，
    帧率 30fps
    私参：preset=ultrafast，tune=zerolatency（低延迟、无 B 帧）
3、avcodec_open2 打开编码器
    准备 AVFrame：
    av_frame_alloc + av_frame_get_buffer
    后续每次填充：Y 平面和 UV 平面从拷贝的节点数据 memcpy 进去
    PTS 处理：从队列取出的 node.pts 是微秒级别的绝对时间戳（从系统启动计时）
    用静态变量 start_pts 记录第一帧的微秒值
    frame->pts = node.pts - start_pts，变成以 0 为起点的相对微秒 PTS
4、编码循环：
    avcodec_send_frame(ctx, frame) 送入原始帧
    avcodec_receive_packet(ctx, pkt) 循环收取压缩包
    返回 0：获得一个 AVPacket
    AVERROR(EAGAIN)：暂无输出，继续喂帧
    AVERROR_EOF：刷新结束
5、刷新编码器：
    avcodec_send_frame(ctx, NULL) 发送空帧，使编码器清空缓冲
    继续 receive_packet 直到 AVERROR_EOF
四、封装阶段（FFmpeg libavformat）
1、创建 MP4 容器： 
    avformat_alloc_output_context2(&fmt_ctx, NULL, "mp4", "test.mp4")
    avformat_new_stream 添加一条视频流
    avcodec_parameters_from_context 将编码器参数复制给流
    容器流时间基设为 {1,90000}（行业标准 90kHz），与编码器的微秒基不同
2、打开文件并写头：
    avio_open(&fmt_ctx->pb, "test.mp4", AVIO_FLAG_WRITE) 创建/打开文件
    avformat_write_header(fmt_ctx, NULL) 写入 MP4 文件头
3、写入数据包：
    对每个收到的 AVPacket：
    fwrite(pkt->data, 1, pkt->size, fout) 保存一份 H.264 裸流
    pkt->stream_index = video_st->index 指定流索引
    av_packet_rescale_ts(pkt, ctx->time_base, video_st->time_base)
    将 PTS 从微秒基转换为 90kHz 基，保证播放时间正确
    av_interleaved_write_frame(fmt_ctx, pkt) 交错写入容器
    每次处理后 av_packet_unref(pkt) 释放内部数据
4、收尾：
    av_write_trailer(fmt_ctx) 写文件尾（修正 moov 等）
    avio_closep(&fmt_ctx->pb) 关闭文件
     释放所有 FFmpeg 资源
