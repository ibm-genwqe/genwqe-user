# Copyright 2015, International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# zlib-devel 1.2.8 is better, but 1.2.7 should work too
#
# The following switch tries to take care that the distros libz.so is been taken:
#    CONFIG_ZLIB_PATH=%{_libdir}/libz.so
# No special libz build should be needed anymore, since we added the right
# dependency to the spec file. We want to have a zlib-devel installed.
# 

Summary: GenWQE userspace tools
Name:    genwqe-tools
Version: 4.0.18
Release: 1%{?dist}
License: Apache-2.0
Group: Development/Tools
URL: https://github.com/ibm-genwqe/genwqe-user/
Requires: zlib >= 1.2.7
BuildRequires: zlib-devel >= 1.2.7 help2man
BuildRoot: %{_tmppath}/%{name}-root
Source0: https://github.com/ibm-genwqe/genwqe-user/archive/v%{version}.tar.gz

%description
Provide a suite of utilities to manage and configure the IBM GenWQE card.

%package -n genwqe-zlib
Summary: GenWQE hardware accelerated libz
Group: System Environment/Base
%description -n genwqe-zlib
GenWQE hardware accelerated libz and test-utilities.

%package -n genwqe-vpd
Summary: GenWQE adapter VPD tools
Group: System Environment/Base
%description -n genwqe-vpd
The genwqe-vpd package contains GenWQE adapter VPD tools.

%package        devel
Summary:        Development files for %{name}
Group:          Development/Libraries
Requires:       %{name} = %{version}

%description devel
The %{name}-devel package contains libraries and header files for
developing applications that use %{name}.

%prep
%setup -q -n genwqe-user-%{version}

%ifarch ppc64le
%define libcxl "BUNDLE_LIBCXL=1"
%endif

%build

%{__make} %{?_smp_mflags} tools lib VERSION=%{version} \
       CONFIG_ZLIB_PATH=%{_libdir}/libz.so.1 %{?libcxl}

%install
%{__make} %{?_smp_mflags} install DESTDIR=%{buildroot}/%{_prefix} \
        VERSION=%{version} SYSTEMD_UNIT_DIR=%{buildroot}/%{_unitdir} \
	LIB_INSTALL_PATH=%{buildroot}/%{_libdir}/genwqe \
	INCLUDE_INSTALL_PATH=%{buildroot}/%{_includedir}/genwqe

# FIXME Instead of trying to fixup things in the spec fike, let us consider
#       changing the associated install rule, such that the spec file
#       can get smaller and simpler.
#

# Move genwqe_vpd.csv to expected location.
%{__mkdir} -p %{buildroot}/%{_sysconfdir}/
%{__install} -m 0644 tools/genwqe_vpd.csv %{buildroot}/etc/

strip %{buildroot}%{_bindir}/genwqe_gzip
strip %{buildroot}%{_bindir}/genwqe_gunzip

ln -sf %{_bindir}/genwqe_gunzip %{buildroot}/%{_libdir}/genwqe/gunzip
ln -sf %{_bindir}/genwqe_gzip   %{buildroot}/%{_libdir}/genwqe/gzip

%files -n genwqe-tools
%defattr(0755,root,root)
%{_bindir}/genwqe_echo
%{_bindir}/genwqe_ffdc
%{_bindir}/genwqe_cksum
%{_bindir}/genwqe_memcopy
%{_bindir}/genwqe_peek
%{_bindir}/genwqe_poke
%{_bindir}/genwqe_update

%{_bindir}/genwqe_gunzip
%{_bindir}/genwqe_gzip
%{_bindir}/genwqe_test_gz
%{_bindir}/genwqe_mt_perf
%{_bindir}/zlib_mt_perf

%{_libdir}/genwqe/gunzip
%{_libdir}/genwqe/gzip

%defattr(-,root,root)
%doc LICENSE
%{_mandir}/man1/genwqe_echo.1.gz
%{_mandir}/man1/genwqe_ffdc.1.gz
%{_mandir}/man1/genwqe_gunzip.1.gz
%{_mandir}/man1/genwqe_gzip.1.gz
%{_mandir}/man1/genwqe_cksum.1.gz
%{_mandir}/man1/genwqe_memcopy.1.gz
%{_mandir}/man1/genwqe_peek.1.gz
%{_mandir}/man1/genwqe_poke.1.gz
%{_mandir}/man1/genwqe_update.1.gz
%{_mandir}/man1/zlib_mt_perf.1.gz
%{_mandir}/man1/gzFile_test.1.gz

%ifarch ppc64le
%{_bindir}/genwqe_maint
%{_bindir}/genwqe_loadtree
%{_unitdir}/genwqe_maint.service
%{_mandir}/man1/genwqe_maint.1.gz
%{_mandir}/man1/genwqe_loadtree.1.gz
%endif

%files -n genwqe-zlib
%defattr(-,root,root)
%doc LICENSE
%defattr(0755,root,root)
%dir %{_libdir}/genwqe
%{_libdir}/genwqe/*.so*

%files -n genwqe-vpd
%defattr(-,root,root,-)
%{_bindir}/genwqe_csv2vpd
%{_bindir}/genwqe_vpdconv
%{_bindir}/genwqe_vpdupdate
%defattr(-,root,root)
%doc LICENSE
%{_sysconfdir}/genwqe_vpd.csv
%{_mandir}/man1/genwqe_csv2vpd.1.gz
%{_mandir}/man1/genwqe_vpdconv.1.gz
%{_mandir}/man1/genwqe_vpdupdate.1.gz

%files devel
%defattr(-,root,root,-)
%dir %{_includedir}/genwqe
%{_includedir}/genwqe/*
%{_libdir}/genwqe/*.a

%changelog
* Thu Jan 05 2017 Frank Haverkamp <haver@linux.vnet.ibm.com> - 4.0.17
- Make Z_STREAM_END detection circumvention configurable
- Improve debug output
- Improve Z_STREAM_END detection and add testcases (most likely not final yet)
* Wed Apr 06 2016 Gabriel Krisman Bertazi <krisman@linux.vnet.ibm.com> - 4.0.16
- dlopen uses SONAME when opening libz.
- Support CAPI version.
- Bulid fixes.
- Include genwqe_maint daemon (CAPI version).
* Mon Apr 04 2016 Frank Haverkamp <haverkam@de.ibm.com>
- Renamed some scripts again
* Thu Feb 04 2016 Frank Haverkamp <haverkam@de.ibm.com>
- Fix s390 and Intel build. Remove debug stuff from zlib rpm.
* Fri Dec 11 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Changing some install directories again.
* Tue Dec 08 2015 Gabriel Krisman Bertazi <krisman@linux.vnet.ibm.com> - 4.0.7-1
- Create Fedora package.
- Make genwqe-vpd and genwqe-libz subpackages of genwqe-tools.
* Wed Apr 22 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
