import unittest
from check_android_frame_pacing import frame_statistics, snapshot_gap, layer_name

class FramePacingTests(unittest.TestCase):
    def test_android_15_surface_name_unwraps_frontend_state(self):
        name='b606f01 SurfaceView[org.rhythmtengoku.mobile/org.rhythmtengoku.mobile.GameActivity](BLAST)#9256'
        self.assertEqual(layer_name('RequestedLayerState{'+name+' parentId=9255}'), name)

    def test_android_12_surface_name_is_unchanged(self):
        name='SurfaceView[org.rhythmtengoku.mobile/org.rhythmtengoku.mobile.GameActivity](BLAST)#304974'
        self.assertEqual(layer_name(name),name)

    def test_snapshot_overlap_proves_continuity(self):
        self.assertFalse(snapshot_gap(list(range(128)), list(range(32,160))))

    def test_wrapped_history_cannot_be_reported_as_game_stutter(self):
        self.assertTrue(snapshot_gap(list(range(128)), list(range(132,260))))

    def test_initial_or_empty_snapshot_is_not_a_ring_wrap(self):
        self.assertFalse(snapshot_gap([], [1,2]))
        self.assertFalse(snapshot_gap([1,2], []))

    @staticmethod
    def frames(seconds):
        return [1_000_000_000+i*16_666_667 for i in range(int(seconds*60)+1)]

    def test_continuous_frames(self):
        self.assertTrue(frame_statistics(self.frames(29),30,'123','123')[0]['passed'])

    def test_freeze_after_initial_smooth_frames(self):
        self.assertFalse(frame_statistics(self.frames(5),30,'123','123')[0]['passed'])

    def test_restart(self):
        self.assertFalse(frame_statistics(self.frames(29),30,'123','456')[0]['passed'])

    def test_stalls(self):
        frames=self.frames(29)[::2]
        self.assertFalse(frame_statistics(frames,30,'123','123')[0]['passed'])

    def test_no_frames(self):
        self.assertFalse(frame_statistics([],30,'123','123')[0]['passed'])

    def test_single_large_stall_cannot_hide_in_average(self):
        frames=self.frames(29)
        frames=[t+(180_000_000 if i>=300 else 0) for i,t in enumerate(frames)]
        self.assertFalse(frame_statistics(frames,30,'123','123')[0]['passed'])

    def test_60_is_not_120(self):
        stats,_=frame_statistics(self.frames(29),30,'123','123',118,.1,17.5,120)
        self.assertFalse(stats['passed'])
        self.assertEqual(100,stats['over_budget_percent'])

    def test_continuous_120(self):
        frames=[1_000_000_000+i*8_333_333 for i in range(29*120+1)]
        self.assertTrue(frame_statistics(frames,30,'123','123',118,.1,17.5,120)[0]['passed'])

    def test_120_repeated_dropped_refreshes(self):
        frames=[1_000_000_000+i*8_333_333 for i in range(29*120+1) if i%300!=1]
        stats,_=frame_statistics(frames,30,'123','123',118,.1,17.5,120)
        self.assertGreater(stats['actual_fps'],118)
        self.assertEqual(stats['over_25ms_percent'],0)
        self.assertFalse(stats['passed'])

if __name__=='__main__':unittest.main()
