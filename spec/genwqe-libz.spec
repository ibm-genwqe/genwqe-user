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

Summary:        GenWQE hardware accelerated libz
Name:           genwqe-libz
Version:	%{srcVersion}

### %{?dist} is the distribution name e.g. el for RHEL
Release:	%{srcRelease}%{?dist}_%(lsb_release -r|cut -f2|sed s/\\./_/g)
License:        Apache License, Version 2.0
Group: 		Development/Tools
Packager:       Frank Haverkamp (haverkam@de.ibm.com)
Source0:        %{name}-%{version}.tgz
Prefix:         /opt/genwqe
BuildRoot:      %{_builddir}/%{name}-%{version}-root
BuildRequires:	zlib

%description
GenWQE hardware accelerated libz and test-utilities.

%prep
%setup
make -s distclean

%build
make -C lib
make -C tools

%install
make -C lib instdir=${RPM_BUILD_ROOT}%{prefix} install_zlib
make -C tools instdir=${RPM_BUILD_ROOT}%{prefix} install_gzip_tools

%clean
make -s distclean

%post

%postun

%files
%defattr(-,root,root,0755)
%{prefix}/lib/genwqe
%{prefix}/bin/genwqe
%{prefix}/include/genwqe

%changelog
* Thu Jan 22 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
