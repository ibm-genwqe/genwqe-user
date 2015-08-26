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

Summary:        GenWQE adapter user-space tools.
Name:           genwqe-tools
Version:	%{srcVersion}

### %{?dist} is the distribution name e.g. el for RHEL
Release:	%{srcRelease}%{?dist}_%(lsb_release -r|cut -f2|sed s/\\./_/g)
License:        Apache License, Version 2.0
Group: 		Development/Tools
Packager:       Frank Haverkamp (haverkam@de.ibm.com)
Source0:        %{name}-%{version}.tgz
Prefix:         /usr
BuildRoot:      %{_builddir}/%{name}-%{version}-root
BuildRequires:	zlib

%description
GenWQE adapter user-space tools.

%prep
%setup
make -s distclean

%build
make -s lib tools BUILD_4TEST=0
make -C tools manpages

%install
make -C tools instdir=${RPM_BUILD_ROOT}%{prefix} \
	install_release_tools install_release_manpages

%clean
make -s distclean

%post

%postun

%files
%defattr(-,root,root,0755)
%{prefix}/bin/*
%{prefix}/man/man1/*

%changelog
* Thu Jan 22 2015 Frank Haverkamp <haverkam@de.ibm.com>
- Initial release.
