#! /bin/bash
#
install_preps() {
    found=`sudo find /etc/ -name "sources.list" | grep -c "list"`
    if [ "found" == "0" ];then
        echo "Currently only distro based on debian release supported"
        exit 1
    fi
    sudo apt-get install  make gcc autoconf automake libtool \
        libtraceevent-dev tar dh-autoreconf
}

configure() {
    autoreconf -vfi 
    ./configure --enable-aer --enable-mce
}

compile() {
    make
}

install_preps
configure
compile
