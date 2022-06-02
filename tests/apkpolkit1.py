# apkpolkit1.py -- apkPolkit1 mock template
#
# Copyright (C) 2022 Pablo Correa Gomez <ablocorrea@hotmail.com>
#
# SPDX-License-Identifier: GPL-2.0+

import dbus
import dbusmock

import time

BUS_NAME   = 'dev.Cogitri.apkPolkit1'
MAIN_OBJ   = '/dev/Cogitri/apkPolkit1'
MAIN_IFACE = 'dev.Cogitri.apkPolkit1'
SYSTEM_BUS = True

def load(mock, parameters):
    repos = [
        (True, "a", "https://alpine.org/alpine/edge/main"),
        (False, "b", "https://pmos.org/pmos/master"),
        (True, "c", "/home/data/foo/bar/baz"),
    ]
    mock.repos = repos

    mock.AddMethods(MAIN_IFACE, [
        ('UpdateRepositories', '', '', ''),
        ('AddRepository', 's', '', ''),
        ('RemoveRepository', 's', '', ''),
        ('ListUpgradablePackages', '', 'a(ssssssttu)', 'ret = [' +
         '("apk-test-app", "0.2.0", "desktop app", "GPL", "0.1.0", "url", 50, 40, 4),' + # 4 = UPGRADABLE
         '("b", "0.2.0", "system package", "GPL", "0.3.0", "url", 50, 40, 5),' + # 5 = DOWNGRADABLE
         ']'),
        ('UpgradePackage', 's', '', ''),
        # We only expect to refine the desktop app for now.
        # Ideally, the state should be updated on the different DBus calls
        ('GetPackageDetails', 's', '(ssssssttu)', 'ret = ' +
         '("apk-test-app", "0.2.0", "desktop app", "GPL", "0.1.0", "url", 50, 40, 2)' # 2 = AVAILABLE
        ),
    ])


@dbus.service.method(MAIN_IFACE, in_signature='s', out_signature='')
def AddPackage(self, pkg_name):
    if (pkg_name == "slow"):
        time.sleep(10)

@dbus.service.method(MAIN_IFACE, in_signature='s', out_signature='')
def DeletePackage(self, pkg_name):
    if (pkg_name == "slow"):
        time.sleep(10)

@dbus.service.method(MAIN_IFACE, in_signature='', out_signature='a(bss)')
def ListRepositories(self):
    return self.repos
