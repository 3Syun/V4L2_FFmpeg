#include<iostream>
#include<fcntl.h>      // 定义 open()、O_RDWR、O_RDONLY、O_WRONLY 等
#include<unistd.h>     // 定义 close()、read()、write() 等系统调用
#include<sys/ioctl.h>   //ioctl()
#include<sys/mman.h> //mmap()
#include<linux/videodev2.h> //v4l2
#include<string.h> //memset
#include<pthread.h>
#include<stdio.h> //printf,perror
#include<stdlib.h> //exit
#include<signal.h>
#include<errno.h>

// FFmpeg 头文件
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavformat/avformat.h>
}


#define VideoDev "/dev/video0"
#define WIDTH 640
#define HEIGHT 480
#define BUFFER_COUNT 4
#define QUEUE_SIZE 8 //队列长度

struct buffers{//将内核缓冲区通过mmap映射到这个上面，用户可以通过这个结构体访问视频帧数据
    void* start;//内存映射后每个缓冲区的起始地址
    size_t length;//缓冲区长度
};

struct buffers usebuf[BUFFER_COUNT];
// 全局退出标志，由信号处理函数设置
static volatile sig_atomic_t quit_flag = 0;



void quit(int sig){
    (void)sig;//显示标记不使用
    quit_flag = 1;
}
//每个帧节点
struct frame_node {
    uint8_t     *data;          // 拷贝后的 NV12 数据
    size_t       bytesused;     // 有效字节数
    int64_t      pts;           // 从 V4L2 timeval 转换成的 PTS
    uint32_t     sequence;      // 帧序列号
    uint32_t     flags;         // V4L2 buf.flags
};
//环形视频帧队列
struct frame_queue{
    struct frame_node nodes[QUEUE_SIZE];
    int read_idx;//读索引
    int write_idx;//写索引
    int count;//当前队列中有效元素的个数，队空消费者需要等待，队满生产者需要等待
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;//条件变量，通知队列非空
    pthread_cond_t  not_full;//通知队列未满
};
void queue_init(struct frame_queue *q){//初始化队列
    memset(q->nodes,0,sizeof(q->nodes));
    q->read_idx = 0;
    q->write_idx = 0;
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}
void queue_destroy(struct frame_queue *q)//销毁队列，释放所有资源
{
    pthread_mutex_lock(&q->lock);

    for(int i = 0; i < q->count; i++)
    {
        int idx = (q->read_idx + i) % QUEUE_SIZE;
        free(q->nodes[idx].data);
        q->nodes[idx].data = NULL;
    }

    pthread_mutex_unlock(&q->lock);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
void queue_wakeup_all(struct frame_queue *q)//退出时，把所有正在睡眠(wait)的线程叫醒
{
    pthread_mutex_lock(&q->lock);

    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);

    pthread_mutex_unlock(&q->lock);
}
//入队（生产者调用），拷贝传入的数据
int queue_put(struct frame_queue *q,uint8_t *data,size_t bytesused,
               struct timeval tv, uint32_t sequence,uint32_t flags){
    pthread_mutex_lock(&q->lock);
    while(q->count == QUEUE_SIZE && !quit_flag){
        //队满则等待（也可选择丢弃）
        pthread_cond_wait(&q->not_full,&q->lock);//not_full为0时，等待并解锁mutex
        //被唤醒后在返回前会重新上锁
    }
    if(quit_flag){
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    //拷贝数据
    struct frame_node *node = &q->nodes[q->write_idx];//指针指向当前写入位置
    //分配内存并拷贝数据
    node->data = (uint8_t*)malloc(bytesused);
    if(node->data == NULL)
    {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    memcpy(node->data,data,bytesused);
    node->bytesused = bytesused;
    //将timeval转换成微妙级的PTS
    node->pts = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;;
    node->sequence = sequence;
    node->flags = flags;

    q->count++;
    q->write_idx = (q->write_idx + 1) % QUEUE_SIZE;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);

    return 0;
}
//出队(消费者消费)，调用者负责释放node->data
int queue_get(struct frame_queue *q,struct frame_node *out){
    pthread_mutex_lock(&q->lock);
    while(q->count == 0 && !quit_flag){
        pthread_cond_wait(&q->not_empty, &q->lock);//队列为空等待唤醒
    }
    if(quit_flag && q->count == 0){
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->nodes[q->read_idx];
     // 所有权转移：清空原节点
    q->nodes[q->read_idx].data = NULL;
    q->nodes[q->read_idx].bytesused = 0;

    q->read_idx = (q->read_idx + 1) % QUEUE_SIZE;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 0;
}
struct frame_queue fq;//全局帧队列

//编码线程
void* encoder_thread(void* args){
    printf("开始编码\n");
    (void)args;
    //1、初始化编码器
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "找不到 H.264 编码器\n");
        return NULL;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);//分配并初始化运行上下文，保存编解码过程中的状态
    if(!ctx){
        fprintf(stderr, "无法分配编码上下文\n");
        return NULL;  
    }
    ctx->width = WIDTH;
    ctx->height =HEIGHT;
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->time_base = (AVRational){1, 1000000};  // 微秒时间基，与 PTS 单位一致
    ctx->framerate = (AVRational){30, 1};
    ctx->bit_rate = 1000000;      // 1 Mbps，可按需调整
    ctx->gop_size = 30;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "打开编码器失败\n");
        avcodec_free_context(&ctx);
        return NULL;
    }
    //2、创建AVFram用于存放原始帧
    AVFrame *frame = av_frame_alloc();//分配结构体
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    av_frame_get_buffer(frame, 0);//为数据分配缓冲区

    AVPacket *pkt = av_packet_alloc();//装压缩的数据包

    FILE *fout = fopen("test.h264", "wb");  // 输出 H.264 裸流
    if (!fout) {
        perror("fopen");
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        return NULL;
    }
    //初始化MP4封装器
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_alloc_output_context2(&fmt_ctx, NULL, "mp4", "test.mp4") < 0 || !fmt_ctx) {
        fprintf(stderr, "无法创建 MP4 输出上下文\n");
        fclose(fout);
        avcodec_free_context(&ctx);
        return NULL;
    }
    AVStream *video_st = avformat_new_stream(fmt_ctx, NULL);
    if (!video_st) {
        fprintf(stderr, "无法创建视频流\n");
        avformat_free_context(fmt_ctx);
        fclose(fout);
        avcodec_free_context(&ctx);
        return NULL;
    }
    //将编码器参数复制到流
    avcodec_parameters_from_context(video_st->codecpar, ctx);
    video_st->time_base = ctx->time_base;
    //打开输出文件并写头部
    if (avio_open(&fmt_ctx->pb, "test.mp4", AVIO_FLAG_WRITE) < 0) {
        fprintf(stderr, "无法打开 test.mp4\n");
        avformat_free_context(fmt_ctx);
        fclose(fout);
        avcodec_free_context(&ctx);
        return NULL;
    }
    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "写 MP4 文件头失败\n");
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        fclose(fout);
        avcodec_free_context(&ctx);
        return NULL;
    }
    while(1){
        struct frame_node node;
        if(queue_get(&fq,&node)<0){
            break;
        }
        //将NV12数据拷贝到AVFrame中
        int y_size = WIDTH*HEIGHT;
        //拷贝Y平面
        memcpy(frame->data[0], node.data, y_size);
        //拷贝UV平面
        memcpy(frame->data[1], node.data + y_size, y_size / 2);
        static int64_t start_pts = AV_NOPTS_VALUE;   // 静态变量，只初始化一次
        if (start_pts == AV_NOPTS_VALUE)
            start_pts = node.pts;
        frame->pts = node.pts - start_pts;//当前帧的绝对时间中减去起点时间，得到相对于第一帧的偏移

        free(node.data);  // 释放拷贝的帧数据
        node.data = NULL; 

        //进入编码器
        int ret = avcodec_send_frame(ctx,frame);//原始帧推入编码器
        if(ret<0){
            fprintf(stderr, "send_frame 错误: %d\n", ret);
            continue;   // 可能会因为时间戳等原因失败，跳过
        }
        //取出编码包
        while(ret>=0){
            ret = avcodec_receive_packet(ctx,pkt);//尝试收取已编码的包
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)//需要更多输入帧 或 编码器已空
                break;
            else if (ret < 0) {
                fprintf(stderr, "receive_packet 错误: %d\n", ret);
                break;
            }
            //写入H.264数据到文件
            fwrite(pkt->data, 1, pkt->size, fout);
            //写入mp4文件
            pkt->stream_index = video_st->index;
            if (av_interleaved_write_frame(fmt_ctx, pkt) < 0) {
                fprintf(stderr, "写入 MP4 失败\n");
            }
            av_packet_unref(pkt);
        }
    }
    //4、刷新编码器
    avcodec_send_frame(ctx, NULL);
    while (1) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret >= 0) {
            fwrite(pkt->data, 1, pkt->size, fout);
            pkt->stream_index = video_st->index;
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    //结束MP4封装
    av_write_trailer(fmt_ctx);
    avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);

    fclose(fout);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);

    printf("[编码线程] 退出\n");
    return NULL;
}
void* cvideo(void* args){
    (void) args;
    //0、查看设备支持的格式 v4l2-ctl --list-formats-ext
    //1、查询当前设备格式 v4l2-ctl --all
    //得出像素640*480 帧率为30 像素格式为NV12
    int fd = open(VideoDev,O_RDWR);
    struct v4l2_format fmt;
    memset(&fmt,0,sizeof(fmt));
    //V4L2 是“驱动决定是否支持单平面/多平面”，不是由像素格式（NV12/YUYV等）单独决定。
    //在V4L2中NV12是单平面采集，NV12M是多平面采集
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;//驱动自动选择合适的图像扫描方式
    //以上四个通常要显示设置，bytesperline和sizeimage由驱动根据以上信息自动填充
    //bytesperline – 每行占用的字节数。 izeimage – 整帧图像占用的总字节数
    if(ioctl(fd,VIDIOC_S_FMT,&fmt)<0){
        perror("格式设置失败！\n");
        return NULL;//exit(EXIT_FAILURE); 会直接退出进程，导致资源还没回收
    }
    printf("========格式设置成功======\n");
    printf("分辨率: %ux%u\n", fmt.fmt.pix.width, fmt.fmt.pix.height);
    printf("扫描方式(field): %u\n", fmt.fmt.pix.field);
    printf("每行字节数(bytesperline): %u\n", fmt.fmt.pix.bytesperline);
    printf("整帧字节数(sizeimage): %u\n", fmt.fmt.pix.sizeimage);
    //3、申请缓冲区
    struct v4l2_requestbuffers req;
    memset(&req,0,sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = fmt.type;
    req.memory = V4L2_MEMORY_MMAP;
    if(ioctl(fd,VIDIOC_REQBUFS,&req)<0){
        perror("缓冲区申请失败！\n");
        return NULL;
    }
    //4、对申请的缓冲区填充信息，并映射到用户区
    for(int i=0;i<BUFFER_COUNT;i++){
        struct v4l2_buffer buf;
        memset(&buf,0,sizeof(buf));
        buf.index = i;
        buf.type = fmt.type;
        buf.memory = req.memory;
        //以上三个必须显示设置，对于单平面，其他内容由驱动填写了
        //注意如果指定了V4L2_MEMORY_DMABUF还要填写导入的dma-buf的文件描述符
        if(ioctl(fd,VIDIOC_QUERYBUF,&buf)<0){
            perror("缓冲区信息填充失败\n");
            return NULL;
        }
        usebuf[i].length = buf.length;
        usebuf[i].start = mmap(NULL,buf.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,buf.m.offset);
        
        printf("buffer[%d] length=%u\n ",i,buf.length);
        //上述步骤完成后，可以直接在用户区通过usebuf[i].start访问到对应的内核缓冲区了
        //同时内核也准备好了BUFFER_COUNT个空闲内核缓冲区buf用来放入视频帧捕获队列，等待硬件填充数据了
        
        // 5. 将申请好的缓冲区放入采集队列（入队队列）
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return NULL;
        }
        printf("mapped and queued\n");
    }
    //6、开始采集视频帧
    ioctl(fd,VIDIOC_STREAMON,&fmt.type);//硬件开始持续输出数据
    printf("开始采集视频帧:\n");
    
    while(!quit_flag){
        struct v4l2_buffer buf;
        memset(&buf,0,sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        //数据在这之前硬件会从入队队列中取出缓冲区自主写完，然后放入完成队列
        ioctl(fd,VIDIOC_DQBUF,&buf);//给DQBUF提供参数，为什么要写BUF？是为后续的QBUF准备正确的参数
        //DQBUF做了以下几件事：
        //1、从完成队列中取出已就绪的缓冲区（将index记录在这里的buf.index中）
        //2、将该缓冲区的其他描述信息填入这里定义的buf
        //3、此时可以通过usebuf[buf.index].start 指针读取视频帧数据了
        //处理完视频帧数据后，需要将这个缓冲区通过VIDIOC_QBUF重新放入视频帧捕获队列

        //7、处理视频帧
        printf("时间戳 %ld.%06ld 帧 %u: 大小 %u 字节\n",
       (long)buf.timestamp.tv_sec,
       (long)buf.timestamp.tv_usec,
       buf.sequence,
       buf.bytesused);
       //buf.timestamp 可以获取视频帧获取的时间，用于同步（音视频对齐，多摄像头融合，控制播放节奏，等），还可以用来录制/回放
       //buf.sequence 可以获取是采集的第几帧（用于健壮性检测，判断是否丢帧，乱序）
       //buf.flages 可以用来判断时钟类型
       //if (buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)表示单调递增时钟，不受系统时间调变影响，常用于同步
       
       //将帧数据入队
        if (queue_put(&fq,
                  (uint8_t*)usebuf[buf.index].start,
                  buf.bytesused,
                  buf.timestamp,
                  buf.sequence,
                  buf.flags) < 0) {
        // 退出信号或内存分配失败
        break;
       }

        //8、重新入队
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    //9、停止视频流
    ioctl(fd, VIDIOC_STREAMOFF, &fmt.type);
    //10、解除映射
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (usebuf[i].start != MAP_FAILED && usebuf[i].start != NULL) {
            munmap(usebuf[i].start, usebuf[i].length);
        }
    }
    close(fd);

    queue_wakeup_all(&fq);   // 唤醒可能阻塞的编码线程
    printf("视频采集线程退出\n");


    return NULL;
    
}

int main(){
    queue_init(&fq);
    //主线程注册信号处理，只用注册一次
    struct sigaction sa;//sa描述信号处理动作
    sa.sa_handler = quit;//指定信号处理函数
    sigemptyset(&sa.sa_mask);//quit执行期间不阻塞额外任何信号，只关注SIGINT和SIGTERM
    sa.sa_flags = 0;//无任何其他特殊行为
    sigaction(SIGINT,  &sa, NULL);//Ctrl+C
    sigaction(SIGTERM, &sa, NULL);//kill

    pthread_t capture_tid, encoder_tid;
    pthread_create(&capture_tid, NULL, cvideo, NULL);
    pthread_create(&encoder_tid, NULL, encoder_thread, NULL);

    // 等待线程结束
    pthread_join(capture_tid, NULL);
    pthread_join(encoder_tid, NULL);

    queue_destroy(&fq);

    printf("程序结束\n");
    return 0;
}