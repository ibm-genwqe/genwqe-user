#
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

Summary:        GenWQE adapter VPD tools
Name:           genwqe-vpd
Version:	%{srcVersion}

### %{?dist} is the distribution name e.g. el for RHEL
Release:	%{srcRelease}%{?dist}_%(lsb_release -r|cut -f2|sed s/\\./_/g)
License:        Apache License, Version 2.0
Group: 		Development/Tools
Packager:       Frank Haverkamp (haverkam@de.ibm.com)
Source0:        %{name}-%{version}.tgz
Prefix:         /usr
BuildRoot:      %{_builddir}/%{name}-%{version}-root
Requires:	glibc glibc-headers gcc binutils

%description
GenWQE adapter VPD tools

%prep
%setup
make clean

%build
make -C tools all manpages

%install
make -C tools instdir=${RPM_BUILD_ROOT}%{prefix} install_vpd_tools
make -C tools instdir=${RPM_BUILD_ROOT}%{prefix} install_vpd_manpages
mkdir -p ${RPM_BUILD_ROOT}/etc
cp tools/genwqe_vpd.csv ${RPM_BUILD_ROOT}/etc/

%clean
make clean

%post

%postun

%files
%defattr(-,root,root,0755)
/etc/*
%{prefix}/bin/*
%{prefix}/man/man1/*

%changelog
* Mon Apr 28 2014 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
