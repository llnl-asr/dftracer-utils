"""The scan schema and the meta describing it must not drift apart."""

from dftracer.utils.dfanalyzer import dfanalyzer_events_meta, hlm_scan_group_by


class TestScanGroupBy:
    def test_keeps_only_columns_the_scan_can_fold_on(self):
        got = hlm_scan_group_by(["time_range", "not_a_column"], ["cat", "io_cat", "func_name"])
        assert got == ["time_range", "cat", "io_cat", "func_name"]

    def test_preserves_order_and_drops_duplicates(self):
        assert hlm_scan_group_by(["cat", "time_range"], ["cat"]) == [
            "cat",
            "time_range",
        ]

    def test_empty_when_nothing_is_groupable(self):
        assert hlm_scan_group_by(["not_a_column"], []) == []


class TestEventsMeta:
    def test_full_grain_matches_the_dfanalyzer_schema(self):
        cols = list(dfanalyzer_events_meta().columns)
        # Order matters: dask compares meta positionally against the scan.
        assert cols[:6] == ["cat", "func_name", "pid", "tid", "file_hash", "host_hash"]
        assert cols[-3:] == ["time_range", "time_start", "time_end"]
        assert "file_nunique" in cols

    def test_grouped_grain_is_group_columns_then_metrics(self):
        gb = ["time_range", "cat", "io_cat"]
        cols = list(dfanalyzer_events_meta(gb).columns)
        assert cols[: len(gb)] == gb
        metrics = cols[len(gb) :]
        assert metrics[0] == "count"
        # The HLM combines these rather than deriving them from raw rows.
        for c in ("time_sq", "size_sq", "time_call_min", "size_call_max"):
            assert c in metrics

    def test_grouped_grain_drops_ungroupable_requests(self):
        cols = list(dfanalyzer_events_meta(["cat", "not_a_column"]).columns)
        assert "not_a_column" not in cols
        assert cols[0] == "cat"

    def test_string_columns_decode_to_str_not_object(self):
        # ipc_to_pandas decodes dictionary columns to pandas str; declaring
        # object here is what made dask reject the partition.
        meta = dfanalyzer_events_meta()
        for col in ("cat", "func_name", "file_name", "proc_name"):
            assert str(meta[col].dtype) == "str"

    def test_nullable_size_columns_are_float(self):
        # size is null for events that carry no transfer, so it arrives float.
        meta = dfanalyzer_events_meta()
        for col in ("size", "size_min", "size_max"):
            assert str(meta[col].dtype) == "float64"
