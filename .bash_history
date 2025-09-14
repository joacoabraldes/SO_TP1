make
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
pkill master view player || true
pkill -x master 2>/dev/null || true
pkill -x view   2>/dev/null || true
pkill -x player 2>/dev/null || true
pgrep -a master   # list matching processes
pgrep -a view
pgrep -a player
# then kill them (careful; only kill what you expect)
pkill -f '/path/to/master'  # matches by command line
pgrep -a master
ls -la /dev/shm
pkill -x master   2>/dev/null || true
pkill -x view     2>/dev/null || true
pkill -x player   2>/dev/null || true
rm -f /dev/shm/game_state /dev/shm/game_sync
ls -la /dev/shm | grep game_
chmod +x ./ChompChamps ./view ./player
pkill -x ChompChamps 2>/dev/null || true
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
ls -l /dev/shm
ls -l /dev/shm | grep SHM_GAME_STATE -n || true
ls -l /dev/shm | grep SHM_GAME_SYNC -n || true
stat /dev/shm/SHM_GAME_STATE
stat /dev/shm/SHM_GAME_SYNC
ls -l /dev/shm
sudo chmod 0666 /dev/shm/game_state /dev/shm/game_sync
chmod 0666 /dev/shm/game_state /dev/shm/game_sync
ls -l /dev/shm
rm /dev/shm/game_state /dev/shm/game_sync
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
./master -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
ls -l /dev/shm
./master -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
ls -l /dev/shm
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
exit
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player

make
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
exit
make clean
exit
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
exit
make
make clean
exit
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
exit
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
make clean
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player}
make clean
exit
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player}
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
exit
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
exit
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make clean
exit
make clean
make
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
make
make clean
./ChompChamps -w 10 -h 10 -d 200 -t 10 -s 123 -v ./view -p ./player ./player
exit
