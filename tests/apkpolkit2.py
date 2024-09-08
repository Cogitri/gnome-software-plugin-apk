# apkpolkit1.py -- apkPolkit1 mock template
#
# Copyright (C) 2022 Pablo Correa Gomez <ablocorrea@hotmail.com>
#
# SPDX-License-Identifier: GPL-2.0+

import dbus
import dbusmock

import time

# These should ideally be directly fetched from C
APK_POLKIT_STATE_AVAILABLE = 0
APK_POLKIT_STATE_INSTALLED = 1
APK_POLKIT_STATE_UPGRADABLE = 4
APK_POLKIT_STATE_DOWNGRADABLE = 5

BUS_NAME   = 'dev.Cogitri.apkPolkit2'
MAIN_OBJ   = '/dev/Cogitri/apkPolkit2'
MAIN_IFACE = 'dev.Cogitri.apkPolkit2'
SYSTEM_BUS = False

def load(mock, parameters):
    repos = [
        (True, "a", "https://alpine.org/alpine/edge/main"),
        (False, "b", "https://pmos.org/pmos/master"),
        (True, "c", "/home/data/foo/bar/baz"),
    ]
    mock.repos = repos

    pkgs = [
        {"name": "apk-test-app",
         "description": "desktop app",
         "installed_size": dbus.UInt64(50),
         "version": "0.1.0-r0",
         "license": "GPL",
         "package_state": dbus.UInt32(APK_POLKIT_STATE_AVAILABLE),
         "size": dbus.UInt64(40),
         "url": "url"},
        {"name": "system-pkg",
         "description": "system package",
         "installed_size": dbus.UInt64(50),
         "version": "2.0-r0",
         "license": "GPL",
         "package_state": dbus.UInt32(APK_POLKIT_STATE_AVAILABLE),
         "size": dbus.UInt64(40),
         "url": "url"},
    ]
    mock.pkgs = pkgs

    mock.AddMethods(MAIN_IFACE, [
        ('AddRepository', 's', '', ''),
        ('RemoveRepository', 's', '', ''),
        ('UpdateRepositories', '', '', 'time.sleep(2)'),
    ])


@dbus.service.method(MAIN_IFACE, in_signature='as', out_signature='')
def AddPackages(self, pkg_list):
    for pkg in pkg_list:
        if pkg == "slow":
            time.sleep(10)

@dbus.service.method(MAIN_IFACE, in_signature='as', out_signature='')
def DeletePackages(self, pkg_list):
    for pkg in pkg_list:
        if pkg == "slow":
            time.sleep(10)

@dbus.service.method(MAIN_IFACE, in_signature='asu', out_signature='aa{sv}')
def GetPackagesDetails(self, packages, requestedProperties):
    apps = []
    # We only expect to refine the desktop app for now.
    # Ideally, the state should be updated on the different DBus calls
    for pkg in packages:
        if pkg == "apk-test-app":
            apps.append(self.pkgs[0])
        else:
            apps.append({"name": pkg, "error": "pkg not found!"})

    return apps

@dbus.service.method(MAIN_IFACE, in_signature='', out_signature='a(bss)')
def ListRepositories(self):
    return self.repos

@dbus.service.method(MAIN_IFACE, in_signature='u', out_signature='aa{sv}')
def ListUpgradablePackages(self, requestedProperties):
    for pkg in self.pkgs:
        if pkg["name"] == "apk-test-app":
            pkg["package_state"] = dbus.UInt32(APK_POLKIT_STATE_UPGRADABLE)
            pkg["staging_version"] = "0.2.0-r0"
        if pkg["name"] == "system-pkg":
            pkg["package_state"] = dbus.UInt32(APK_POLKIT_STATE_DOWNGRADABLE)
            pkg["staging_version"] = "0.1.0-r0"
    return self.pkgs


@dbus.service.method(MAIN_IFACE, in_signature='as', out_signature='')
def UpgradePackages(self, packages):
    for pkg in packages:
        for p in self.pkgs:
            if p["name"] == pkg:
                p["version"] = p["staging_version"]
                p["package_state"] = dbus.UInt32(APK_POLKIT_STATE_INSTALLED)

@dbus.service.method(MAIN_IFACE, in_signature='asu', out_signature='aa{sv}')
def SearchFilesOwners(self, paths, requestedProperties):
    pkgs = []
    for path in paths:
        if path == "/usr/share/apps/no-source-app.desktop":
            pkgs.append({"name": "no-source-pkg"})
        else:
            pkgs.append({})
    return pkgs
