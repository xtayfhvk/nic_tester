# nic_tester

##  功能描述
    通过socket + ipv4对目的设备的端口进行测试,  如果通畅, 则返回当前网卡的信息, 
    不需要制定网卡信息,  程序会自动的遍历所有的网卡, 
    程序没有使用多线程,  使用epoll技术, 等待多个socket的响应,  来确认网络端口是否通畅, 
     

##  运行环境 php 5.6.40   

    是否是nts看当前PhP.

##  编译过程

    /usr/local/php5.6/bin/phpize 
    
    ./configure  --with-php-config=/usr/local/php5.6/bin/php-config  --enable-nic_tester

    make && make install


#  使用过程
    将编译好的 modules/nic_tester.so 复制到php的扩展库目录下, 记得修改Php.ini文件

#  函数测试 
    
    ``` 测试84.117 设备  80端口是否可以通过socket连接通畅.  等待时间2S. 任意物理接口都无法访问.
    /usr/local/php5.6/bin/php -r "print_r(nic_tester_check('172.16.84.117', 80, 2));"
    Array
    (
    )

    ``` 测试84.117 设备  443 端口是否可以通过socket连接通畅.  等待时间2S. 返回3个接口的信息. 
    /usr/local/php5.6/bin/php -r "print_r(nic_tester_check('172.16.84.117', 443, 2));"
    Array
    (
        [0] => Array
            (
                [name] => ens33
                [ip] => 172.16.84.129
            )

        [1] => Array
            (
                [name] => dummy0
                [ip] => 172.16.84.121
            )

        [2] => Array
            (
                [name] => macvlan0
                [ip] => 172.16.84.122
            )

    )

