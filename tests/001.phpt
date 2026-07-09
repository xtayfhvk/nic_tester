--TEST--
Check for nic_tester extension presence and IPv6 validation
--SKIPIF--
<?php if (!extension_loaded("nic_tester")) print "skip"; ?>
--FILE--
<?php
echo "nic_tester extension is available\n";

/* 验证地址校验: 无效地址应返回 FALSE */
var_dump(nic_tester_check("127.0.0.1", 80));
var_dump(nic_tester_check("::1", 80));
var_dump(nic_tester_check("224.0.0.1", 80));
var_dump(nic_tester_check("ff02::1", 80));
var_dump(nic_tester_check("0.0.0.0", 80));
var_dump(nic_tester_check("::", 80));
var_dump(nic_tester_check("not-an-address", 80));

/* 验证 IPv4-mapped 绕过防护 */
var_dump(nic_tester_check("::ffff:127.0.0.1", 80));
var_dump(nic_tester_check("::ffff:224.0.0.1", 80));

/* 验证有效地址不返回 FALSE (返回 array, 可能为空) */
$r = nic_tester_check("172.16.84.117", 443, 2);
echo "Valid result is array: " . (is_array($r) ? "yes" : "NO") . "\n";

/* 如果有结果, 检查新格式 */
if (count($r) > 0) {
    $nic = $r[0];
    echo "Has name: " . (isset($nic["name"]) ? "yes" : "NO") . "\n";
    echo "Has ipv4: " . (isset($nic["ipv4"]) ? "yes" : "NO") . "\n";
    echo "Has ipv6: " . (isset($nic["ipv6"]) ? "yes" : "NO") . "\n";
    echo "Old 'ip' key gone: " . (isset($nic["ip"]) ? "STILL_PRESENT" : "yes") . "\n";
    echo "ipv4 is array: " . (is_array($nic["ipv4"]) ? "yes" : "NO") . "\n";
    echo "ipv6 is array: " . (is_array($nic["ipv6"]) ? "yes" : "NO") . "\n";
}
?>
--EXPECT--
nic_tester extension is available
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
bool(false)
Valid result is array: yes
