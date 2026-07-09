# nic_tester

##  功能描述
    通过 socket 对目的设备的端口进行测试 (支持 IPv4 / IPv6 双栈),
    如果通畅, 则返回当前网卡的信息。
    不需要指定网卡信息, 程序会自动遍历所有的网卡,
    采用 AF_INET6 双栈 socket (IPV6_V6ONLY=0) 统一处理两种协议族,
    IPv4 目标通过 IPv4-mapped 地址 (::ffff:x.x.x.x) 连接。
    跨协议族可达性由内核决定, 代码不做预判。
    程序使用 epoll (Linux) / select (Windows) 技术,
    等待多个 socket 的响应, 来确认网络端口是否通畅。

##  运行环境 php 5.6.40

    是否是nts看当前PHP.

##  编译过程

    /usr/local/php5.6/bin/phpize

    ./configure  --with-php-config=/usr/local/php5.6/bin/php-config  --enable-nic_tester

    make && make install

##  返回值格式 (v2.0.0)

    返回一个索引数组, 每个元素是一个关联数组:
    {
        name: 接口名称,
        ipv4: [IPv4地址列表],
        ipv6: [IPv6地址列表]
    }
    只包含至少有一个连接成功的接口。
    当目标地址无效 (非标准 unicast) 时返回 FALSE。

#  使用过程
    将编译好的 modules/nic_tester.so 复制到php的扩展库目录下, 记得修改php.ini文件

#  函数测试

    ``` 测试 IPv4 目标 172.16.84.117 的 80 端口, 等待 2s, 无接口可达.
    /usr/local/php5.6/bin/php -r "print_r(nic_tester_check('172.16.84.117', 80, 2));"
    Array
    (
    )

    ``` 测试 IPv4 目标 172.16.84.117 的 443 端口, 等待 2s, 返回 3 个接口.
    /usr/local/php5.6/bin/php -r "print_r(nic_tester_check('172.16.84.117', 443, 2));"
    Array
    (
        [0] => Array
            (
                [name] => ens33
                [ipv4] => Array
                    (
                        [0] => 172.16.84.129
                    )
                [ipv6] => Array
                    (
                        [0] => fe80::20c:29ff:fe5c:1234
                    )
            )
        [1] => Array
            (
                [name] => dummy0
                [ipv4] => Array
                    (
                        [0] => 172.16.84.121
                    )
                [ipv6] => Array
                    (
                    )
            )
    )

    ``` 测试 IPv6 目标 2001:db8::1 的 443 端口
    /usr/local/php5.6/bin/php -r "print_r(nic_tester_check('2001:db8::1', 443, 2));"

    ``` 无效地址返回 FALSE
    /usr/local/php5.6/bin/php -r "var_dump(nic_tester_check('127.0.0.1', 80));"
    bool(false)
    /usr/local/php5.6/bin/php -r "var_dump(nic_tester_check('::1', 80));"
    bool(false)
    /usr/local/php5.6/bin/php -r "var_dump(nic_tester_check('224.0.0.1', 80));"
    bool(false)
