#!/usr/bin/env python
# Copyright (c) 2014 Daniel Kraft
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# RPC test for basic name registration.

# Add python-bitcoinrpc to module search path:
import os
import sys
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), "python-bitcoinrpc"))

from bitcoinrpc.authproxy import JSONRPCException
from names import NameTestFramework
from util import assert_equal

class NameRegistrationTest (NameTestFramework):

  def run_test (self, nodes):
    # TODO: call super class

    # Perform name_new's.  Check for too long names exception.
    newA = nodes[0].name_new ("node-0")
    newB = nodes[1].name_new ("node-1")
    nodes[0].name_new ("x" * 255)
    try:
      nodes[0].name_new ("x" * 256)
      raise AssertionError ("too long name not recognised by name_new")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -8)
    self.generate (nodes, 0, 5)

    # first_update the names.  Check for too long values.
    addrA = nodes[0].getnewaddress ()
    txidA = self.firstupdateName (nodes[0], "node-0", newA, "value-0", addrA)
    try:
      self.firstupdateName (nodes[1], "node-1", newB, "x" * 521)
      raise AssertionError ("too long value not recognised by name_firstupdate")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -8)
    self.firstupdateName (nodes[1], "node-1", newB, "x" * 520)
    
    # Check that the name appears when the name_new is ripe.
    self.generate (nodes, 0, 7)
    try:
      nodes[1].name_show ("node-0")
      raise AssertionError ("name available when it should not yet be")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -4)
    self.generate (nodes, 0, 1)

    data = self.checkName (nodes[1], "node-0", "value-0", 30, False)
    assert_equal (data['address'], addrA)
    assert_equal (data['txid'], txidA)
    assert_equal (data['height'], 213)

    # Check for error with rand mismatch (wrong name)
    newA = nodes[0].name_new ("test-name")
    self.generate (nodes, 0, 10)
    try:
      self.firstupdateName (nodes[0], "test-name-wrong", newA, "value")
      raise AssertionError ("wrong rand value not caught by name_firstupdate")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -25)

    # Check for mismatch with prev tx from another node for name_firstupdate
    # and name_update.
    try:
      self.firstupdateName (nodes[1], "test-name", newA, "value")
      raise AssertionError ("wrong node can firstupdate a name")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -4)
    self.firstupdateName (nodes[0], "test-name", newA, "test-value")

    # Check for disallowed firstupdate when the name is active.
    newSteal = nodes[1].name_new ("node-0")
    self.generate (nodes, 0, 19)
    self.checkName (nodes[1], "node-0", "value-0", 1, False)
    try:
      self.firstupdateName (nodes[1], "node-0", newSteal, "stolen")
      raise AssertionError ("name stolen before expiry")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -25)

    # Check for "stealing" of the name after expiry.
    self.generate (nodes, 0, 1)
    self.firstupdateName (nodes[1], "node-0", newSteal, "stolen")
    self.checkName (nodes[1], "node-0", "value-0", 0, True)
    self.generate (nodes, 0, 1)
    self.checkName (nodes[1], "node-0", "stolen", 30, False)

    # Check basic updating.
    try:
      nodes[0].name_update ("test-name", "x" * 521)
      raise AssertionError ("update to too long value allowed")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -8)
    nodes[0].name_update ("test-name", "x" * 520)
    self.checkName (nodes[0], "test-name", "test-value", None, False)
    self.generate (nodes, 0, 1)
    self.checkName (nodes[1], "test-name", "x" * 520, 30, False)

    addrB = nodes[1].getnewaddress ()
    nodes[0].name_update ("test-name", "sent", addrB)
    self.generate (nodes, 0, 1)
    data = self.checkName (nodes[0], "test-name", "sent", 30, False)
    assert_equal (data['address'], addrB)
    nodes[1].name_update ("test-name", "updated")
    self.generate (nodes, 0, 1)
    data = self.checkName (nodes[0], "test-name", "updated", 30, False)

    # Invalid updates.
    try:
      nodes[1].name_update ("wrong-name", "foo")
      raise AssertionError ("invalid name updated")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -25)
    try:
      nodes[0].name_update ("test-name", "stolen?")
      raise AssertionError ("wrong node could update sent name")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -4)
    
    # Update failing after expiry.  Re-registration possible.
    self.checkName (nodes[1], "node-1", "x" * 520, None, True)
    try:
      nodes[1].name_update ("node-1", "updated?")
      raise AssertionError ("expired name updated")
    except JSONRPCException as exc:
      assert_equal (exc.error['code'], -25)

    newSteal = nodes[0].name_new ("node-1")
    self.generate (nodes, 0, 10)
    self.firstupdateName (nodes[0], "node-1", newSteal, "reregistered")
    self.generate (nodes, 0, 10)
    self.checkName (nodes[1], "node-1", "reregistered", 23, False)

if __name__ == '__main__':
  NameRegistrationTest ().main ()
