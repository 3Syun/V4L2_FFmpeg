#include<stdio.h>
#include<alsa/asoundlib.h>

#define SAMPLE_RATE 44100  //采样率 
#define CHANNELS 2 //通道数
#define PERIOD_SIZE 512//每次读多少帧进行编码
#define BUFFER_SIZE 4096 //编码缓冲区总大小

int main(){
    snd_pcm_t *pcm_handle;//定义一个对声卡（麦克风）进行操作的句柄
    snd_pcm_hw_params_t *hw_params;//定义一个参数的配置表
    int ret;
    //1、打开默认的录音设备，第三个参数指定的是录音还是播放，此处是录音，SND_PCM_STREAM_PLAYBACK是播放
    ret = snd_pcm_open(&pcm_handle,"default",SND_PCM_STREAM_CAPTURE,0);
    if(ret < 0){
        fprintf(stderr,"设备打开失败\n",snd_strerror(ret));
        return 1;
    }
    //2、配置参数
    snd_pcm_hw_params_alloca(&hw_params);//在栈上分配并初始化一个snd_pcm_hw_params_t 结构体（分配一个空的参数表）
    snd_pcm_hw_params_any(pcm_handle,hw_params);//初始化硬件参数对象，给参数表填充该 PCM 设备所支持的全部参数范围，相当于把硬件参数配置的“自由度”调到最大

    snd_pcm_hw_params_set_access(pcm_handle,hw_params,SND_PCM_ACCESS_RW_INTERLEAVED);//指定应用程序与 PCM 设备之间传输音频样本的访问（这里是最常用的交错读写方式）
    snd_pcm_hw_params_set_format(pcm_handle,hw_params,SND_PCM_FORMAT_S16_LE);//指定音频样本的表示格式——即每个采样点是多长的、有无符号、字节序如何（此处是有符号16位，小段字节序的线性PCM格式）
    snd_pcm_hw_params_set_channels(pcm_handle,hw_params,CHANNELS);

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm_handle,hw_params,&rate,0);//最后一个参数指示实际采样率与期望值的关系
    /*
    near表示当你传入 val = 44100，但声卡只支持 8000, 16000, 48000 时会自动找到最近的值，两个最近的找最大的
    最后一个参数若不为 NULL，返回：
    • -1：实际值 < 期望值
    • 0：实际值 == 期望值
    • 1：实际值 > 期望值
    */

    snd_pcm_uframes_t period = PERIOD_SIZE;
    snd_pcm_uframes_t BUFFER = BUFFER_SIZE;
    snd_pcm_hw_params_set_period_size_near(pcm_handle,hw_params,&period,0);
    snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &BUFFER);

    ret = snd_pcm_hw_params(pcm_handle, hw_params);
    if(ret < 0){
        fprintf(stderr,"参数设置失败：%s\n",snd_strerror(ret));
        return 1;
    }

    //3、准备一个缓冲区，用来存放一次读取数据
    //一个帧大小 = 通道数*每个样本字节数（此处设置为了16bit,即2字节）
    int frame_bytes = CHANNELS*2;
    char *buffer = (char*)malloc(period* frame_bytes);

    //4、打开输出文件，先写入WAV头（简化起见这里先不写头，直接写裸 PCM）
    FILE* out = fopen("mic.pcm","wb");

    //循环读取1000次（大约100*512 / 44100 = 11.6秒）
    for(int i=0;i<1000;i++){
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_handle,buffer,period);// PCM 录音设备以交错模式读取指定帧数的音频数据到用户缓冲区，返回实际读取的帧数或错误码
        if(frames<0){
            frames = snd_pcm_recover(pcm_handle, frames, 0);//将PCM流从XRUN状态中恢复过来
            if(frames<0){
                fprintf(stderr, "读取错误: %s\n", snd_strerror(frames));
                break;
            }
            continue;
        }
        //写入文件，写入的内存地址、每一项的字节数、项的个数、指向已打开的音频文件
        fwrite(buffer,frame_bytes,frames,out);
    }
        free(buffer);
    fclose(out);
    snd_pcm_close(pcm_handle);
    return 0;

}