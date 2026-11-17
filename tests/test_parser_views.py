#!/usr/bin/env python3
"""Black-box checks for SQL-backed token views and escaped-value lifetimes."""
import tempfile
import unittest

from test_engine import Session, literal


class ParserViewTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='mvdb-token-views-')
        self.db = Session(self.directory.name)

    def tearDown(self):
        self.db.stop()
        self.directory.cleanup()

    def test_decoded_literals_survive_token_growth_and_future_requests(self):
        q = self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(256),v VECTOR(2));')
        values = []
        for i in range(1000):
            # Mix SSO and heap-backed decoded strings, Unicode, embedded NUL and quotes.
            text = ("'\\" + '中文😀' + '\0\n\t') * (1 + i % 12)
            vector = "'[1.25,-2.5]'" if i % 2 else '[1.25,-2.5]'
            values.append((i, text, vector))
        q('INSERT INTO t VALUES '+','.join(f'({i},{literal(text)},{vector})' for i,text,vector in values)+';')
        for _ in range(3):
            q('SELECT id FROM t WHERE id=500;')
        self.assertEqual(q('SELECT id,s FROM t ORDER BY id;')['rows'], [[i,text] for i,text,_ in values])
        self.db.restart(kill=True)
        self.assertEqual(q('SELECT id,s FROM t ORDER BY id;')['rows'], [[i,text] for i,text,_ in values])

    def test_quotes_comments_and_empty_strings(self):
        q = self.db.request
        q('cReAtE TABLE `Ta``ble` (`Id` BIGINT PRIMARY KEY, "Co""l" VARCHAR(64));')
        q("INSERT INTO `Ta``ble` VALUES (1,''),(2,'a''b'),(3,'a\\\\b'),(4,'-- /* ; */ FROM');")
        self.assertEqual(q('SELECT * FROM `Ta``ble` ORDER BY id;')['rows'],
                         [[1,''],[2,"a'b"],[3,'a\\b'],[4,'-- /* ; */ FROM']])
        self.assertEqual(q('SELECT * FROM `Ta``ble` LIMIT 0;')['columns'], ['Id','Co"l'])

    def test_integer_limits_and_type_errors(self):
        q = self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,n BIGINT);')
        q('INSERT INTO t VALUES (- /* comment */ 9223372036854775808,+ 9223372036854775807);')
        self.assertEqual(q('SELECT * FROM t;')['rows'], [[-2**63,2**63-1]])
        for integer in ('9223372036854775808','-9223372036854775809','18446744073709551616'):
            q(f'INSERT INTO t VALUES (1,{integer});', error='value_out_of_range')
        for integer in ('1.0','1e2'):
            q(f'INSERT INTO t VALUES (1,{integer});', error='type_error')
        self.assertEqual(q('SELECT id FROM t;')['rows'], [[-2**63]])

    def test_failed_statement_discards_borrowed_and_decoded_values(self):
        q = self.db.request
        q('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(16));')
        q('BEGIN;')
        q("INSERT INTO t VALUES (1,'keep');")
        q("INSERT INTO t VALUES (2,'escaped''x'),(3,'normal'),(1,'duplicate');", error='duplicate_key')
        q('COMMIT;')
        self.db.restart(kill=True)
        self.assertEqual(q('SELECT * FROM t;')['rows'], [[1,'keep']])


if __name__ == '__main__':
    unittest.main(verbosity=2)
