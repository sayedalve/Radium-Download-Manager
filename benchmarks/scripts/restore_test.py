with open('tests/cpp/unit_tests.cpp', 'rb') as f:
    data = f.read()

old_b = b'openidm::brisk::SegmentScheduler scheduler(total, 8, 4194304ULL);'
new_b = b'openidm::brisk::SegmentScheduler scheduler(total, 8, 262144);'

assert old_b in data, "old_b not found"
data = data.replace(old_b, new_b, 1)

with open('tests/cpp/unit_tests.cpp', 'wb') as f:
    f.write(data)
print("Restored unit_tests.cpp successfully")
