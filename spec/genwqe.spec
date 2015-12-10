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

Summary: GenWQE userspace tools
Name:    genwqe-tools
Version: 4.0.7
Release: 1%{?dist}
License: Apache license
Group: Development/Tools
URL: https://github.com/ibm-genwqe/genwqe-user/

Requires: zlib >= 1.2.8
BuildRequires: zlib-devel >= 1.2.8

#Source0: https://github.com/ibm-genwqe/genwqe-user/archive/%{version}.tar.gz
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
%{__make} %{?_smp_mflags} tools lib VERSION=%{version}

%install
%{__make} %{?_smp_mflags} install instdir=%{buildroot}/%{_prefix} VERSION=%{version}

# Move genwqe_vpd.csv to expected location.
%{__mkdir} -p %{buildroot}/%{_sysconfdir}/
%{__install} -m 0644 tools/genwqe_vpd.csv %{buildroot}/etc/

# Move man pages to expected location.
%{__mkdir} -p %{buildroot}/%{_mandir}/man1
%{__mv} %{buildroot}/usr/man/man1/* %{buildroot}/%{_mandir}/man1

# Install example programs into /usr/lib/genwqe/
%{__mkdir} -p %{buildroot}/%{_libdir}/%{name}
%{__mv} %{buildroot}/usr/bin/genwqe/gunzip %{buildroot}/%{_libdir}/%{name}/
%{__mv} %{buildroot}/usr/bin/genwqe/gzip %{buildroot}/%{_libdir}/%{name}/
%{__mv} %{buildroot}/usr/bin/genwqe/zlib_mt_perf.sh %{buildroot}/%{_libdir}/%{name}/
%{__mv} %{buildroot}/usr/bin/genwqe/zlib_test_gz.sh %{buildroot}/%{_libdir}/%{name}/

# Remove programs we don't want to package.
%{__rm} %{buildroot}/usr/bin/genwqe/zlib_mt_perf

%files
%doc LICENSE
%defattr(0755,root,root)
%{_bindir}/genwqe_echo
%{_bindir}/genwqe_update
%{_bindir}/genwqe_memcopy
%{_bindir}/genwqe_cksum
%{_bindir}/genwqe_ffdc
%{_bindir}/genwqe_gunzip
%{_bindir}/genwqe_gzip
%{_bindir}/genwqe_loadtree
%{_bindir}/genwqe_maint
%{_bindir}/genwqe_peek
%{_bindir}/genwqe_poke
%{_bindir}/zlib_mt_perf
%{_libdir}/%{name}/gunzip
%{_libdir}/%{name}/gzip

%{_libdir}/%{name}/zlib_mt_perf.sh
%{_libdir}/%{name}/zlib_test_gz.sh

%{_mandir}/man1/genwqe_cksum.1.gz
%{_mandir}/man1/genwqe_echo.1.gz
%{_mandir}/man1/genwqe_ffdc.1.gz
%{_mandir}/man1/genwqe_gunzip.1.gz
%{_mandir}/man1/genwqe_gzip.1.gz
%{_mandir}/man1/genwqe_loadtree.1.gz
%{_mandir}/man1/genwqe_maint.1.gz
%{_mandir}/man1/genwqe_memcopy.1.gz
%{_mandir}/man1/genwqe_peek.1.gz
%{_mandir}/man1/genwqe_poke.1.gz
%{_mandir}/man1/genwqe_update.1.gz
%{_mandir}/man1/zlib_mt_perf.1.gz

%files -n genwqe-zlib
%defattr(0755,root,root)
%{_prefix}/lib/genwqe
%{_prefix}/lib/
%{_prefix}/include/genwqe
%{_prefix}/include/

%files -n genwqe-vpd
%{_sysconfdir}/genwqe_vpd.csv
%{_bindir}/csv2bin
%{_bindir}/genwqe_vpdconv
%{_bindir}/genwqe_vpdupdate
%{_mandir}/man1/csv2bin.1.gz
%{_mandir}/man1/genwqe_vpdconv.1.gz
%{_mandir}/man1/genwqe_vpdupdate.1.gz

%changelog
* Tue Dec 08 2015 Gabriel Krisman Bertazi <krisman@linux.vnet.ibm.com> - 4.0.7-1
- Create Fedora package.
- Make genwqe-vpd and genwqe-libz subpackages of genwqe-tools.
* Wed Apr 22 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
