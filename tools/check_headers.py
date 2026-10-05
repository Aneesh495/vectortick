#!/usr/bin/env python3
import argparse, concurrent.futures, json, pathlib, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',default='c++');p.add_argument('--json',default='headers.json');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
headers=sorted((root/'include').rglob('*.hpp'))
def check(h):
    with tempfile.TemporaryDirectory() as d:
        src=pathlib.Path(d)/'header.cpp';src.write_text('#include "'+h.relative_to(root/'include').as_posix()+'"\nint main() {}\n')
        command=[a.compiler,'-std=c++20','-fno-exceptions','-fno-rtti','-Wall','-Wextra','-Wpedantic','-Werror','-fsyntax-only','-I'+str(root/'include'),str(src)]
        r=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        return {'header':str(h.relative_to(root)),'exit_code':r.returncode,'output':r.stdout}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool: results=list(pool.map(check,headers))
pathlib.Path(a.json).write_text(json.dumps({'compiler':a.compiler,'headers_checked':len(results),'failures':sum(x['exit_code']!=0 for x in results),'results':results},indent=2)+'\n')
for r in results:
    if r['exit_code']: print(r['header']+'\n'+r['output'])
print('Headers checked:',len(results));raise SystemExit(not results or any(r['exit_code'] for r in results))
