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

# Verbose level:
#   V=0 means completely silent
#   V=1 means brief output
#   V=2 means full output
V		?= 2

ifeq ($(V),0)
	Q		:= @
	MAKEFLAGS	+= --silent
	MAKE		+= -s
endif

ifeq ($(V),1)
	MAKEFLAGS	+= --silent
	MAKE		+= -s
endif

version = $(shell git describe --abbrev=4 --dirty --always --tags)

instdir = /opt/genwqe

distro = $(shell lsb_release -d | cut -f2)
subdirs += lib tools

# We do not want to build our driver for ancient distributions ...
ifneq ($(distro),Red Hat Enterprise Linux Client release 5.6 (Tikanga))
#ifneq ($(distro),Red Hat Enterprise Linux Workstation release 6.2 (Santiago))
#ifneq ($(distro),Red Hat Enterprise Linux Workstation release 6.4 (Santiago))
subdirs += driver
#endif
#endif
endif

targets += $(subdirs)

UDEV_RULES_D ?= /etc/udev/rules.d
MODPROBE_D ?= /etc/modprobe.d

all: $(targets)

.PHONY: $(subdirs) clean build_dist install uninstall 	\
	install_src uninstall_src copy_test_code \
	install_test_code build_testcode

# Only build if the subdirectory is really existent
$(subdirs):
	@if [ -d $@ ]; then			\
		$(MAKE) -C $@ C=0 || exit 1;	\
	fi

rpmbuild_setup:
	@mkdir -p ~/rpmbuild/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	$(RM) ~/.rpmmacros
	echo '%_topdir %(echo $$HOME)/rpmbuild' >  ~/.rpmmacros

rpmbuild: genwqe-tools genwqe-libz genwqe-test-package genwqe-vpd

genwqe-tools genwqe-driver genwqe-libz:
	@$(MAKE) -s distclean
	@$(RM) -r /tmp/$@-$(version) /tmp/$@-$(version).tgz  \
		~/rpmbuild/SOURCES/${@}* ~/rpmbuild/BUILD/${@}* \
		~/tmp/$@-$(version)
	@mkdir -p /tmp/$@-$(version)
	@cp -ar * /tmp/$@-$(version)/
	@find /tmp/$@-$(version)/ \
		-depth -name 'CVS' -exec rm -rf '{}' \; -print
	(cd /tmp && tar cfz $@-$(version).tgz $@-$(version))
	@cp /tmp/$@-$(version).tgz ~/rpmbuild/SOURCES/
	@cp spec/$@.spec ~/rpmbuild/SPECS/
	rpmbuild -ba -v --define 'srcVersion $(version)' \
		--define 'srcRelease 1'			\
		--buildroot ~/tmp/$@-$(version)		\
		~/rpmbuild/SPECS/$@.spec

genwqe-vpd:
	@$(MAKE) -s distclean
	@$(RM) -r /tmp/$@-$(version) /tmp/$@-$(version).tgz  \
		~/rpmbuild/SOURCES/${@}* ~/rpmbuild/BUILD/${@}* \
		~/tmp/$@-$(version)
	@mkdir -p /tmp/$@-$(version)
	@cp -ar  etc/genwqe_vpd.csv			\
		include/genwqe_vpd.h			\
		tools/genwqe_vpd_common.c		\
		tools/genwqe_vpd_converter.c		\
		tools/genwqe_tools.h			\
		/tmp/$@-$(version)/
	@cp -ar  spec/genwqe_vpdconv.mk	/tmp/$@-$(version)/Makefile
	(cd /tmp && tar cfz $@-$(version).tgz $@-$(version))
	@cp /tmp/$@-$(version).tgz ~/rpmbuild/SOURCES/
	@cp spec/$@.spec ~/rpmbuild/SPECS/
	rpmbuild -ba -v --define 'srcVersion $(version)' \
		--define 'srcRelease 1'			\
		--buildroot ~/tmp/$@-$(version)		\
		~/rpmbuild/SPECS/$@.spec

genwqe-test-package:
	@$(RM) -r ~/rpmbuild/SOURCES/${@}* ~/rpmbuild/BUILD/${@}*
	@mkdir -p /tmp/$@-$(version)
	@cp -ar  tests/zedc/scripts/test_gz.sh          \
		tests/zedc/data/testdata.tar.gz              \
		/tmp/$@-$(version)/
	(cd /tmp && tar cfz $@-$(version).tgz $@-$(version))
	@cp /tmp/$@-$(version).tgz ~/rpmbuild/SOURCES/
	@cp spec/$@.spec ~/rpmbuild/SPECS/
	rpmbuild -bb -v --define 'srcVersion $(version)' \
		--define 'srcRelease 1'                 \
		--buildroot ~/tmp/$@-$(version)         \
		~/rpmbuild/SPECS/$@.spec
    # clean up temporary data again
	@$(RM) -r /tmp/$@-$(version) /tmp/$@-$(version).tgz  \
              /tmp/$@-$(version) ~/tmp

# Install/Uninstall
install uninstall:
	@for dir in $(subdirs); do 			\
		if [ -d $$dir ]; then			\
			$(MAKE) -C $$dir $@ || exit 1;	\
		fi					\
	done

install_udev_rules:
	mkdir -p $(UDEV_RULES_D)
	cp etc/udev/rules.d/52-genwqedevices.rules $(UDEV_RULES_D)/

uninstall_udev_rules:
	$(RM) $(UDEV_RULES_D)/52-genwqedevices.rules

install_modprobe_d:
	mkdir -p $(MODPROBE_D)
	cp etc/modprobe.d/genwqe.conf $(MODPROBE_D)/

uninstall_modprobe_d:
	$(RM) $(MODPROBE_D)/genwqe.conf

install_src uninstall_src:
	@if [ -z $(KERNELSRC) ]; then					\
		echo "Please set KERNELSRC. Aborting.";			\
	elif [ -z $(GENWQETOOLSSRC) ]; then				\
		echo "Please set GENWQETOOLSSRC. Aborting.";		\
	else								\
		if [ $@ == "install_src" ]; then			\
			mkdir -p $(KERNELSRC)/include/linux/uapi/linux/genwqe/;	\
			cp include/linux/uapi/linux/genwqe/*.h		\
				$(KERNELSRC)/include/linux/uapi/linux/genwqe/;\
			mkdir -p $(GENWQETOOLSSRC)/include/linux/genwqe/;\
			cp include/*.h $(GENWQETOOLSSRC)/include/;	\
			cp include/linux/uapi/linux/genwqe/*.h		\
				$(GENWQETOOLSSRC)/include/linux/genwqe/;\
			cp Makefile config.mk $(GENWQETOOLSSRC);        \
			for dir in $(subdirs); do			\
				$(MAKE) -C $$dir $@ || exit 1;		\
			done;						\
		else							\
			rm -f $(KERNELSRC)/include/linux/uapi/linux/genwqe/*.h;	\
			rm -f $(GENWQETOOLSSRC)/include/linux/genwqe/*.h;\
			rm -f $(GENWQETOOLSSRC)/include/*.h;		\
			rm -f $(GENWQETOOLSSRC)/Makefile;		\
			for dir in $(subdirs); do			\
				$(MAKE) -C $$dir $@ || exit 1;		\
			done;						\
		fi							\
	fi

# FIXME This is a problem which occurs on distributions which have a
# genwqe_card.h header file which is different from our local one.  If
# there is a better solution than renaming it to get it out of the way
# please fix this.
#
fixup_headerfile_problem:
	@if [ -f /usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h ]; then \
		sudo mv /usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h \
			/usr/src/kernels/`uname -r`/include/uapi/linux/genwqe/genwqe_card.h.orig ; \
	fi

# Rules for making clean:
clean:
	@for dir in $(subdirs); do 			\
		if [ -d $$dir ]; then			\
			$(MAKE) -C $$dir $@ || exit 1;	\
		fi					\
	done
	@$(RM) *~ */*~ \
		GenWQETools-$(version).tgz \
		GenWQELibZ-$(version).tgz \
		GenWQEVpd-$(version).tgz \
		/tmp/genwqe-test-package-$(version).tgz
	@find . -depth -name '*~'  -exec rm -rf '{}' \; -print
	@find . -depth -name '.#*' -exec rm -rf '{}' \; -print

distclean: clean
	@$(RM) -r sim_*
