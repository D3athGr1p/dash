#!/usr/bin/env python3
# Copyright (c) 2015-2021 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_rotation.py

Checks LLMQs Quorum Rotation

'''

from test_framework.test_framework import DashTestFramework
from test_framework.util import connect_nodes, isolate_node, reconnect_isolated_node, sync_blocks, assert_equal, \
    assert_greater_than_or_equal, wait_until


def intersection(lst1, lst2):
    lst3 = [value for value in lst1 if value in lst2]
    return lst3


def extract_quorum_members(quorum_info):
    return [d['proTxHash'] for d in quorum_info["members"]]


class LLMQQuorumRotationTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(31, 30, fast_dip3_enforcement=True)
        self.set_dash_llmq_test_params(4, 4)

    def run_test(self):

        # Connect all nodes to node1 so that we always have the whole network connected
        # Otherwise only masternode connections will be established between nodes, which won't propagate TXs/blocks
        # Usually node0 is the one that does this, but in this test we isolate it multiple times

        for i in range(len(self.nodes)):
            if i != 1:
                connect_nodes(self.nodes[i], 0)

        self.activate_dip8()

        self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        self.activate_dip24()
        self.log.info("Activated DIP24 at height:" + str(self.nodes[0].getblockcount()))

        cycle_length = 24

        #At this point, we need to move forward 3 cycles (3 x 24 blocks) so the first 3 quarters can be created (without DKG sessions)
        #self.log.info("Start at H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H+C height:" + str(self.nodes[0].getblockcount()))
        self.move_to_next_cycle(cycle_length)
        self.log.info("Cycle H+2C height:" + str(self.nodes[0].getblockcount()))

        (quorum_info_0_0, quorum_info_0_1) = self.mine_cycle_quorum()
        quorum_members_0_0 = extract_quorum_members(quorum_info_0_0)
        quorum_members_0_1 = extract_quorum_members(quorum_info_0_1)
        assert_equal(len(intersection(quorum_members_0_0, quorum_members_0_1)), 0)
        self.log.info("Quorum #0_0 members: " + str(quorum_members_0_0))
        self.log.info("Quorum #0_1 members: " + str(quorum_members_0_1))

        (quorum_info_1_0, quorum_info_1_1) = self.mine_cycle_quorum()
        quorum_members_1_0 = extract_quorum_members(quorum_info_1_0)
        quorum_members_1_1 = extract_quorum_members(quorum_info_1_1)
        assert_equal(len(intersection(quorum_members_1_0, quorum_members_1_1)), 0)
        self.log.info("Quorum #1_0 members: " + str(quorum_members_1_0))
        self.log.info("Quorum #1_1 members: " + str(quorum_members_1_1))

        (quorum_info_2_0, quorum_info_2_1) = self.mine_cycle_quorum()
        quorum_members_2_0 = extract_quorum_members(quorum_info_2_0)
        quorum_members_2_1 = extract_quorum_members(quorum_info_2_1)
        assert_equal(len(intersection(quorum_members_2_0, quorum_members_2_1)), 0)
        self.log.info("Quorum #2_0 members: " + str(quorum_members_2_0))
        self.log.info("Quorum #2_1 members: " + str(quorum_members_2_1))

        assert_greater_than_or_equal(len(intersection(quorum_members_0_0, quorum_members_1_0)), 3)
        assert_greater_than_or_equal(len(intersection(quorum_members_0_1, quorum_members_1_1)), 3)

        assert_greater_than_or_equal(len(intersection(quorum_members_0_0, quorum_members_2_0)), 2)
        assert_greater_than_or_equal(len(intersection(quorum_members_0_1, quorum_members_2_1)), 2)

        assert_greater_than_or_equal(len(intersection(quorum_members_1_0, quorum_members_2_0)), 3)
        assert_greater_than_or_equal(len(intersection(quorum_members_1_1, quorum_members_2_1)), 3)

    def move_to_next_cycle(self, cycle_length):
        mninfos_online = self.mninfo.copy()
        nodes = [self.nodes[0]] + [mn.node for mn in mninfos_online]
        cur_block = self.nodes[0].getblockcount()

        # move forward to next DKG
        skip_count = cycle_length - (cur_block % cycle_length)
        if skip_count != 0:
            self.bump_mocktime(1, nodes=nodes)
            self.nodes[0].generate(skip_count)
        sync_blocks(nodes)
        self.log.info('Moved from block %d to %d' % (cur_block, self.nodes[0].getblockcount()))


if __name__ == '__main__':
    LLMQQuorumRotationTest().main()