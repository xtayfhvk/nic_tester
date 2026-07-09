PHP_ARG_ENABLE([nic_tester],
  [whether to enable nic_tester extension],
  [AS_HELP_STRING([--enable-nic_tester],
    [Enable nic_tester support])])

if test "$PHP_NIC_TESTER" != "no"; then
  PHP_NEW_EXTENSION(nic_tester, nic_tester.c, $ext_shared)
  AC_DEFINE(HAVE_NIC_TESTER, 1, [Have nic_tester extension])
fi
