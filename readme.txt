About this project:
   This project can successfully help to transcode any format video file, switch it to HLS protocol and generate TS and m3u8 file, 
   user can use video player to play this generated TS file by m3u8, so that to play any video files which are not supported, 
   it not only support Arm + X86 platform and can use ffmpeg HW acceleration during Decode, scale, and encode, but also support 
   ffmpeg pure software transcoding. currently, we defaultly use H264 for video target encoder and AAC for audio target encoder while generate
   HLS protocol and TS file if not specified in command line.
For detail: 
   1： when in Arm platform: using RKMPP decoder, rga scale, H264_rkmpp encoder to implement a hardware transcoding solution
   2： when in X86 platform: using VAAPI decoder, vaapi scale, H264_VAAPI encoder to implement a hardware transcoding solution
   3:  if hardware decoder or encoder or scale failed during transcoding in Arm or X86 platform, it can automatically switch to 
       software solution to replace it.
   4:  it not only support ffmpeg api transode, but also support using ffmpeg elf transcode, we can made some changes in 
       CMakeList.txt to flexibly switch between them
	 
About how to use it:
    1： we can Create a build file inside current in the current path to compile this project by using: camke .. && make
        and then get the target zetMediaServer file which can be used in X86 platform
    2： we can also create a build-arm64 file inside current in the current path to compile this project by using: 
        camke .. && make -DENABLE_ARM_CROSS_COMPILE=ON to get the target zetMediaServer file which can be used in Arm platform

For example, by executing this command:
			./zetMediaServer -i inputfile.mp4 -w 640 -h 480 -b 2500 -a aac  -ab 128 -s 44100 -v h265 -hls 
	this command can help to transcode inputfile.mp4 to generate 640*480 resolutions and 2.5M bitrate for H265 video, 
	and 128k bitrate 44.1k sampleRate for AAC Audio in HLS file.

In addition:
	-HLS parameter is essential, and other parameters can be specified arbitrarily. If these are not specified, 
	the audio and video encoding parameters from the original video will be used.