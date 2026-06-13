# V4L2_FFmpeg

编译：
g++ viode.cc -o test     $(pkg-config --cflags --libs libavcodec libavformat libavutil)     -lpthread
