<?php
echo "=== IPv4 target ===\n";
$r = nic_tester_check('172.16.84.117', 443, 2);
print_r($r);

if (count($r) > 0) {
    echo "\n=== Format verification ===\n";
    $nic = $r[0];
    echo "name: " . $nic['name'] . "\n";
    echo "ipv4: "; print_r($nic['ipv4']);
    echo "ipv6: "; print_r($nic['ipv6']);
    echo "Old 'ip' key present: " . (isset($nic['ip']) ? 'YES (regression)' : 'no (good)') . "\n";
}

echo "\n=== Address validation ===\n";
echo "127.0.0.1: "; var_dump(nic_tester_check('127.0.0.1', 80));
echo "::1: ";       var_dump(nic_tester_check('::1', 80));
echo "224.0.0.1: "; var_dump(nic_tester_check('224.0.0.1', 80));
echo "ff02::1: ";   var_dump(nic_tester_check('ff02::1', 80));
echo "0.0.0.0: ";   var_dump(nic_tester_check('0.0.0.0', 80));
echo "::ffff:127.0.0.1: "; var_dump(nic_tester_check('::ffff:127.0.0.1', 80));
echo "::ffff:224.0.0.1: "; var_dump(nic_tester_check('::ffff:224.0.0.1', 80));

echo "\n=== IPv6 target ===\n";
$r6 = nic_tester_check('2001:db8::1', 443, 2);
echo "Result: ";
print_r($r6);

echo "\n=== Default params ===\n";
$r_def = nic_tester_check('8.8.8.8');
echo "Type: " . gettype($r_def) . ", count: " . count($r_def) . "\n";
