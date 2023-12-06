#!bin/sh

sudo rm -rf ./test/autobahn/reports && cd ./test/autobahn && sudo docker run -it --rm \
    -v "${PWD}/config:/config"  -v "${PWD}/reports:/reports" \
    --network="host"     \
    --name fuzzingclient \
     crossbario/autobahn-testsuite \
     wstest -m fuzzingclient --spec /config/fuzzingclient.json \

