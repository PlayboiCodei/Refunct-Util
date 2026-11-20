"""Guard against synchronous button-history disk IO in the character tick."""
from pathlib import Path
p = Path(__file__).resolve().parents[1] / 'src' / 'practice.cpp'
source = p.read_text(encoding='utf-8')

track = source[source.index('void TrackLastButton('):source.index('void UpdatePractice(')]
assert track.count('PersistButtonHistory();') == 2
assert 'WritePrivateProfileStringW(' not in track

persist = source[source.index('void PersistButtonHistory()'):source.index('// Refunct\'s older UE4', source.index('void PersistButtonHistory()'))]
assert 'if (QueueButtonHistorySnapshot(path)) return;' in persist
assert 'WriteButtonHistorySnapshot(snapshot)' in persist  # failure fallback
assert 'WritePrivateProfileStringW(' not in persist

load = source[source.index('void LoadButtonHistoryOnce()'):source.index('void PersistButtonHistory()')]
assert 'StartButtonHistoryWriter();' in load

writer = source[source.index('DWORD WINAPI ButtonHistoryWriterMain('):source.index('void StartButtonHistoryWriter()')]
assert 'WaitForSingleObject(worker->wake, INFINITE)' in writer
assert 'WriteButtonHistorySnapshot(snapshot)' in writer
assert 'worker->pending = false;' in writer
assert 'Sleep(50)' in writer

commit = source[source.index('bool WriteButtonHistorySnapshot('):source.index('DWORD WINAPI ButtonHistoryWriterMain(')]
assert 'FlushFileBuffers(file)' in commit
assert 'MoveFileExW(temporary.c_str(), snapshot.path.c_str(),' in commit
assert 'MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH' in commit
assert 'L"[History]\\r\\nVersion=1\\r\\nValid=1\\r\\nLast="' in commit
assert 'EncodeButtonContact(snapshot.second)' in commit
print('PASS: button history now queued to atomic background snapshot writer')
