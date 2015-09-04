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
rpmversion = $(shell git describe --abbrev=0)

instdir = /opt/genwqe

distro = $(shell lsb_release -d | cut -f2)
subdirs += lib tools
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

rpmbuild: genwqe-tools genwqe-libz genwqe-vpd

genwqe-tools genwqe-libz genwqe-vpd:
	@$(MAKE) -s distclean
	@$(RM) -r /tmp/$@-$(rpmversion) /tmp/$@-$(rpmversion).tgz  \
		~/rpmbuild/SOURCES/${@}* ~/rpmbuild/BUILD/${@}* \
		~/tmp/$@-$(rpmversion)
	@mkdir -p /tmp/$@-$(rpmversion)
	@cp -ar .git /tmp/$@-$(rpmversion)/
	@cp -ar * /tmp/$@-$(rpmversion)/
	(cd /tmp && tar cfz $@-$(rpmversion).tgz $@-$(rpmversion))
	@cp /tmp/$@-$(rpmversion).tgz ~/rpmbuild/SOURCES/
	@cp spec/$@.spec ~/rpmbuild/SPECS/
	rpmbuild -ba -v --define 'srcVersion $(rpmversion)' \
		--define 'srcRelease 1'			\
		--buildroot ~/tmp/$@-$(rpmversion)	\
		~/rpmbuild/SPECS/$@.spec

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
	@$(RM) *~ */*~ 					\
		genwqe-tools-$(rpmversion).tgz 		\
		genwqe-zlib-$(rpmversion).tgz
	@find . -depth -name '*~'  -exec rm -rf '{}' \; -print
	@find . -depth -name '.#*' -exec rm -rf '{}' \; -print

distclean: clean
	@$(RM) -r sim_*
