# example/storeless/Makefile - a storeless product (cbase/app.h storeless) on cbase, local alone, so
# the platform's smokes run against an app that keeps no store. cbase's own check runs
# `make -C example/storeless smoke` (the top-level Makefile's storeless-smoke): smoke_boot then
# takes its storeless branch.
#
#   make -C example/storeless smoke   build/cbase and the platform's smokes against it
CBASE := ../..
CBASE_PROVIDERS := local
APP_SRC_ROOTS := src
APP_TEST_ROOTS :=
include $(CBASE)/cbase.mk
