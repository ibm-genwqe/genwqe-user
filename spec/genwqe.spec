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

BuildRequires: zlib
BuildRequires: zlib-devel

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
%{__make} %{?_smp_mflags} tools lib

%install
%{__make} %{?_smp_mflags} -C lib install_zlib  instdir=%{buildroot}/%{_prefix}
%{__make} %{?_smp_mflags} -C tools install_release_tools install_vpd_tools \
          install_vpd_manpages instdir=%{buildroot}/%{_prefix}

# Move genwqe_vpd.csv to expected location.
%{__mkdir} -p %{buildroot}/%{_sysconfdir}/
%{__install} -m 0644 tools/genwqe_vpd.csv %{buildroot}/etc/

# Move man pages to expected location.
%{__mkdir} -p %{buildroot}/%{_mandir}/man1
%{__mv} %{buildroot}/usr/man/man1/* %{buildroot}/%{_mandir}/man1

%files
%doc LICENSE
%defattr(0755,root,root)
%{_bindir}/genwqe_echo
%{_bindir}/genwqe_update
%{_bindir}/genwqe_memcopy

%files -n genwqe-zlib
%defattr(0755,root,root)
%{_prefix}/lib/genwqe
%{_prefix}/include/genwqe

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
