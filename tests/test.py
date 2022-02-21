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
        klass.dbus_con = klass.get_dbus(system_bus=True)

    def setUp(self):
        self.log = open(os.getenv('DBUS_TEST_LOG'), "w")
        self.p_mock = self.spawn_server('dev.Cogitri.apkPolkit1',
                                        '/dev/Cogitri/apkPolkit1',
                                        'dev.Cogitri.apkPolkit1',
                                        system_bus=True,
                                        stdout=self.log)

        self.apk_polkit_mock = dbus.Interface(self.dbus_con.
                                              get_object('dev.Cogitri.apkPolkit1',
                                                         '/dev/Cogitri/apkPolkit1'),
                                              MOCK_IFACE)

        self.apk_polkit_mock.AddMethods('', [
            ('AddPackage', 's', '', ''),
            ('DeletePackage', 's', '', '')
        ])


    def tearDown(self):
        self.log.close()
        self.p_mock.terminate()
        self.p_mock.wait()

    def test_apk(self):
        builddir = os.getenv('G_TEST_BUILDDIR')
        test_file = os.path.join(builddir, 'gs-self-test-apk')
        result = subprocess.run(test_file)
        self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    _test = unittest.TextTestRunner(stream=sys.stdout)
    unittest.main(testRunner=_test)
