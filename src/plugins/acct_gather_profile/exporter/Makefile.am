# Makefile for acct_gather_profile/exporter plugin

AUTOMAKE_OPTIONS = foreign

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -DSLURM_PLUGIN_DEBUG -I$(top_srcdir) -I$(top_srcdir)/src/common

pkglib_LTLIBRARIES = acct_gather_profile_exporter.la

acct_gather_profile_exporter_la_SOURCES = acct_gather_profile_exporter.c
acct_gather_profile_exporter_la_LDFLAGS = $(SO_LDFLAGS) $(PLUGIN_FLAGS)
