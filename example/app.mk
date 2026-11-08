# example/app.mk - the example product, as a Makefile that includes cbase.mk describes it. Included
# by cbase's own Makefile (EXAMPLE := example/) and by example/Makefile (EXAMPLE empty), so the
# app is described once.
APP_SRC_ROOTS   := $(EXAMPLE)src
APP_TEST_ROOTS  := $(EXAMPLE)test
APP_INC         := -I$(EXAMPLE)src
APP_SMOKES      := smoke_example
APP_GATE_HEADER := example_app.h
APP_LAMBDA_SCHEDULE := tick
APP_LAMBDA_JOB  := greet
