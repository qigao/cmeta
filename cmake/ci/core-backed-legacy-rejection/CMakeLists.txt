cmake_minimum_required(VERSION 3.20)
project(SaltsLegacyEpochRejection LANGUAGES C)
# This intentionally requests the legacy ABI, which the 3.0 package
# MUST reject. The caller fails CI if this configure succeeds.
find_package(Salts 2.3 EXACT CONFIG REQUIRED)
