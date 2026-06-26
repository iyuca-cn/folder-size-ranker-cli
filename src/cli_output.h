#ifndef MFTSCAN_CLI_OUTPUT_H
#define MFTSCAN_CLI_OUTPUT_H

#include "model.h"

MftscanError mftscan_cli_output_table(const MftscanOptions *options, const MftscanScanResult *scan_result);
MftscanError mftscan_cli_output_json(const MftscanOptions *options, const MftscanScanResult *scan_result);
MftscanError mftscan_cli_output_all_json(const MftscanOptions *options, const MftscanContext *context);

#endif
