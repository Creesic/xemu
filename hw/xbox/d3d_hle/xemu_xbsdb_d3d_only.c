/*
 * Xemu registers only the D3D8/D3D8LTCG libraries with XbSymbolDatabase.
 * The upstream core keeps references to its other optional databases in one
 * table, so provide empty definitions instead of compiling several megabytes
 * of unrelated audio/network/API signatures into the emulator.
 */
#include "OOVPA_databases.h"

OOVPATableList DSound_OOVPA_manual;
OOVPATableList DSound_OOVPA;
OOVPATableList JVSLIB_OOVPA;
OOVPATableList XACTENG_OOVPA;
OOVPATableList XAPILIB_OOVPA;
OOVPATableList XGRAPHC_OOVPA;
OOVPATableList XNET_OOVPA;
OOVPATableList XONLINE_OOVPA;
