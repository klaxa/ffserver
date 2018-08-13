#!/bin/bash

# This simple test generates a silent 60s 720p test video and streams it with ffserver.
# At the time of writing incorrect timestamps prevent HLS from working properly and
# something else is preventing DASH from working properly. However the matroska test
# should succeed.
# The tests need ffserver to be compiled and ffmpeg and curl to be present.


# generate source
ffmpeg -re -f lavfi -i testsrc2=s=1280x720:r=30 -f lavfi -i anullsrc=r=48000 -g 60 -c:v libx264 -c:a aac -pix_fmt yuv420p -t 60 -y orig.mkv -g 60 -c:v libx264 -c:a aac -pix_fmt yuv420p -t 60 -listen 1 -f matroska http://127.0.0.1:8080 2> source.log &
ffmpeg_pid=$!

sleep 1

# start server
./ffserver test_config.lua 2> ffserver.log &
ffserver_pid=$!

sleep 2

ffmpeg -i http://127.0.0.1:8081/test/mkv -y -c copy mkv.mkv 2> mkv.log &
ffmpeg_mkv_pid=$!

sleep 1

curl http://127.0.0.1:8081/test/mkv > curl_mkv.mkv 2> curl_mkv.log &
curl_mkv_pid=$!

#ffmpeg -i http://127.0.0.1:8081/test/hls -y -c copy hls.mkv 2> hls.log &
#ffmpeg_hls_pid=$!

#ffmpeg -i http://127.0.0.1:8081/test/dash -y -c copy dash.mkv 2> dash.log &
#ffmpeg_dash_pid=$!


wait $ffmpeg_pid
echo source quit

wait $ffmpeg_mkv_pid
echo mkv quit

wait $curl_mkv_pid
echo curl mkv quit

#wait $ffmpeg_hls_pid
#echo hls quit

#wait $ffmpeg_dash_pid
#echo dash quit

wait $ffserver_pid
echo ffserver quit


md5sum orig.mkv mkv.mkv curl_mkv.mkv

rm -rf test/

ffmpeg -i orig.mkv -f framecrc -y orig.crc
ffmpeg -i mkv.mkv -f framecrc -y mkv.crc
ffmpeg -i curl_mkv.mkv -f framecrc -y curl_mkv.crc

echo Diffs:

fail=0

diff orig.crc mkv.crc
if [[ $? -ne "0" ]]
then
    echo orig.mkv differs from mkv.mkv
    fail=1
fi
diff orig.crc curl_mkv.crc
if [[ $? -ne "0" ]]
then
    echo orig.mkv differs from curl_mkv.mkv
    fail=1
fi

if [[ $fail -eq "0" ]]
then
    echo Test passed, files are the same.
fi

rm orig.mkv orig.crc mkv.mkv mkv.crc curl_mkv.mkv curl_mkv.crc source.log ffserver.log mkv.log curl_mkv.log
