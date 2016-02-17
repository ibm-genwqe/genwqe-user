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

# FIXME How can we automatically set the version? Version changes from time
#       to time, would be good if we could autoadjust this to avoid us editing
#       the spec file on any version increase. Let me try %Version to fix that.
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
Version: %Version
Release: 1%{?dist}
License: Apache license
Group: Development/Tools
URL: https://github.com/ibm-genwqe/genwqe-user/
Requires: zlib >= 1.2.7
BuildRequires: zlib-devel >= 1.2.7
Source0: https://github.com/ibm-genwqe/genwqe-user/archive/genwqe-%{version}.tar.gz

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
GenWQE adapter VPD tools

%prep
%setup -q -n genwqe-%{version}

%build
%{__make} %{?_smp_mflags} tools lib VERSION=%{version} \
	CONFIG_ZLIB_PATH=%{_libdir}/libz.so

%install
%{__make} %{?_smp_mflags} install DESTDIR=%{buildroot}/%{_prefix} VERSION=%{version}

#
# FIXME Instead of trying to fixup things in the spec fike, let us consider
#       changing the associated install rule, such that the spec file
#       can get smaller and simpler.
#

# Move genwqe_vpd.csv to expected location.
%{__mkdir} -p %{buildroot}/%{_sysconfdir}/
%{__install} -m 0644 tools/genwqe_vpd.csv %{buildroot}/etc/

# Move man pages to expected location.
%{__mkdir} -p %{buildroot}/%{_mandir}/man1
%{__mv} %{buildroot}/usr/man/man1/* %{buildroot}/%{_mandir}/man1

%files -n genwqe-tools
%doc LICENSE
%defattr(0755,root,root)
%{_bindir}/genwqe_*
%{_bindir}/zlib_mt_perf
%{_bindir}/genwqe_mt_perf
%{_bindir}/genwqe_test_gz
%{_bindir}/genwqe/gunzip
%{_bindir}/genwqe/gzip

%{_mandir}/man1/genwqe_*.gz
%{_mandir}/man1/zlib_mt_perf.1.gz

%files -n genwqe-zlib
%doc LICENSE
%defattr(0755,root,root)
%{_prefix}/lib/genwqe/*
%{_prefix}/include/genwqe/*

%files -n genwqe-vpd
%doc LICENSE
%{_sysconfdir}/genwqe_vpd.csv
%{_bindir}/genwqe_csv2vpd
%{_bindir}/genwqe_vpdconv
%{_bindir}/genwqe_vpdupdate
%{_mandir}/man1/genwqe_csv2vpd.1.gz
%{_mandir}/man1/genwqe_vpdconv.1.gz
%{_mandir}/man1/genwqe_vpdupdate.1.gz

%changelog
* Thu Feb 04 2016 Frank Haverkamp <haverkam@de.ibm.com>
- Fix s390 and Intel build. Remove debug stuff from zlib rpm.
* Fri Dec 11 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Changing some install directories again.
* Tue Dec 08 2015 Gabriel Krisman Bertazi <krisman@linux.vnet.ibm.com> - 4.0.7-1
- Create Fedora package.
- Make genwqe-vpd and genwqe-libz subpackages of genwqe-tools.
* Wed Apr 22 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
