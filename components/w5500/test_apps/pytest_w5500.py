# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
Target test using EthTestRunner from eth_test_app component.
"""

import pytest

from idf_build_apps.constants import IDF_VERSION
from packaging.version import Version
from pytest_embedded import Dut

TEST_IF = ''

# W5500 driver v2 requires IDF >= 6.0.
_W5500_REQUIRES_IDF6 = pytest.mark.skipif(
    IDF_VERSION < Version('6.0'),
    reason='W5500 driver v2 requires IDF >= 6.0',
)


@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default_w5500', 'esp32', marks=[pytest.mark.eth_w5500, _W5500_REQUIRES_IDF6]),
        pytest.param('poll_w5500', 'esp32', marks=[pytest.mark.eth_w5500, _W5500_REQUIRES_IDF6]),
    ],
    indirect=['target'],
)
def test_eth_w5500(dut: Dut, eth_test_runner) -> None:
    eth_test_runner.run_ethernet_test_apps(dut)
    dut.serial.hard_reset()
    eth_test_runner.run_ethernet_l2_test(dut, TEST_IF)
    dut.serial.hard_reset()
    eth_test_runner.run_ethernet_heap_alloc_test(dut, TEST_IF)
