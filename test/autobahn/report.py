import json
import sys


argC = len(sys.argv)
if argC < 3:
    print("Invalid arguments: please provide report file path and server name")
    exit(1)


filepath = sys.argv[1]

serverName = sys.argv[2]

f = open(filepath)
reportData = f.read()


report = json.loads(reportData)


failed = 0

for case, result in report[serverName].items():
    if result["behavior"] != "OK" and result["behavior"] != "NON-STRICT" and result["behavior"] != "INFORMATIONAL":
        failed = 1


if failed == 0:
    print("[PASS] Autobahn testsuite")
else:
    print("[FAIL] Autobahn testsuite")


exit(failed)
