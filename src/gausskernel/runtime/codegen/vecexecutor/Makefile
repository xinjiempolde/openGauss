#
#Copyright (c) 2020 Huawei Technologies Co.,Ltd.
#
#openGauss is licensed under Mulan PSL v2.
#You can use this software according to the terms and conditions of the Mulan PSL v2.
#You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
#-------------------------------------------------------------------------
#
# Makefile
#    Makefile for codegen vecexecutor
#
# IDENTIFICATION
#    src/gausskernel/runtime/codegen/vecexecutor/Makefile
#
#-------------------------------------------------------------------------

subdir = src/gausskernel/runtime/codegen/vecexecutor
top_builddir = ../../../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS:=-std=c++17 $(CPPFLAGS)
ifneq "$(MAKECMDGOALS)" "clean"
  ifneq "$(MAKECMDGOALS)" "distclean"
    ifneq "$(shell which g++ |grep hutaf_llt |wc -l)" "1"
      -include $(DEPEND)
    endif
  endif
endif
OBJS = vecexprcodegen.o vechashaggcodegen.o vechashjoincodegen.o vecsortcodegen.o

# append include directory about zlib1.2.7
override CPPFLAGS += -I$(LIBLLVM_INCLUDE_PATH) -I$(top_builddir)/contrib/hdfs_fdw/orc/include -D_DEBUG -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -O2 -fomit-frame-pointer -fvisibility-inlines-hidden -fno-exceptions -fno-rtti  -L$(LIBLLVM_LIB_PATH) -lz -pthread -D_REENTRANT -lncurses -lrt -ldl -lm $(LLVM_LIBS)
override mywarningflags:=-Wall -Werror -Wendif-labels -Wformat-security -Wmissing-format-attribute -Wpointer-arith -Wno-write-strings -Wno-attributes -Wno-unused-but-set-variable
override CPPFLAGS:=$(filter-out $(mywarningflags),$(CPPFLAGS))

include $(top_srcdir)/src/gausskernel/common.mk

###########################################
# Set the warning option of compiler
###########################################

override CXXFLAGS:=$(filter-out $(mywarningflags),$(CXXFLAGS))
override CXXFLAGS+=$(mywarningflags)
