#!/usr/bin/env python3
#
# Copyright (C) 2022 Pablo Correa Gomez <ablocorrea@hotmail.com>
#
# SPDX-License-Identifier: GPL-2.0+

import dbus
from dbusmock import DBusTestCase, MOCK_IFACE
import os
import subprocess
import sys
import unittest

class GsPluginApkTest (DBusTestCase):
    @classmethod
    def setUpClass(klass):
        klass.start_system_bus()

    def setUp(self):
        self.log = None
        if os.getenv('DBUS_TEST_LOG') is not None:
            self.log = open(os.getenv('DBUS_TEST_LOG'), "w")
        template = os.path.join(os.getenv('G_TEST_SRCDIR'), 'apkpolkit2.py')
        (self.p_mock, _) = self.spawn_server_template(template,
                                                      stdout=self.log)

    def tearDown(self):
        if self.log is not None:
            self.log.close()

    def test_apk(self):
        builddir = os.getenv('G_TEST_BUILDDIR')
        test_file = os.path.join(builddir, 'gs-self-test-apk')
        result = subprocess.run(test_file)
        self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    _test = unittest.TextTestRunner(stream=sys.stdout)
    unittest.main(testRunner=_test)
