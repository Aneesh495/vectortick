#!/usr/bin/env python3
"""Bind executed local and hosted verification to unchanged source bytes."""
import argparse, copy, gzip, hashlib, io, json, pathlib, re, shutil, subprocess, sys, tarfile, tempfile, xml.etree.ElementTree as ET
from run_verification import ROOT, source_manifest, digest, dump, semantic, semhash
REQUIRED=[f'{os}-{compiler}-{kind}' for os in ('ubuntu-24.04','ubuntu-24.04-arm') for compiler in ('gcc','clang') for kind in ('Debug','Release')]+['macos-15-clang-Debug','macos-15-clang-Release','ubuntu-24.04-clang-sanitizers','ubuntu-24.04-clang-tsan']
def require(ok,msg):
 if not ok:raise ValueError(msg)
def load_archive(path):
 with tarfile.open(path,'r:gz') as t:
  members=t.getmembers();require(all(m.isfile() and not pathlib.PurePosixPath(m.name).is_absolute() and '..' not in pathlib.PurePosixPath(m.name).parts for m in members),'Unsafe artifact member')
  return {m.name:t.extractfile(m).read() for m in members}
def validate_job(files,manifest,revision):
 require('job.json' in files,'Missing job record');job=json.loads(files['job.json']);require(job['status']=='PASSED','Failed job');require(job['revision']==revision,'Stale job revision');require(job['source_manifest']==manifest,'Mismatched job source')
 require(job['artifact_manifest'],'Missing raw artifacts')
 for n,h in job['artifact_manifest'].items():require(n in files and hashlib.sha256(files[n]).hexdigest()==h,'Missing or changed artifact: '+n)
 require(set(files)==set(job['artifact_manifest'])|{'job.json'},'Unbound artifact files')
 def data(n):require(n in files,'Missing '+n);return json.loads(files[n])
 commands={r['name']:r for r in job['commands']};required={'configure','build','headers','diff-check'}
 if job['tsan']:required.add('crc-tsan')
 else:required|={'ctest','empty-filter','native-campaign','sql-campaign','demo','generate-pcap','benchmark'}|{'invalid-'+str(i) for i in range(7)}|{'pcap-short-header','pcap-short-packet','pcap-excessive-snaplen','pcap-inconsistent-packet','vtp-checksum'}
 require(required<=set(commands),'Missing required commands')
 for r in job['commands']:
  require(r['log'] in files and files[r['log']],'Missing raw command log');require(r['wall_ns']>0,'Invalid command timing');require(r['exit_code']==r['expected_exit'] or r['expected_exit']=='nonzero' and r['exit_code']!=0,'Failed required command')
 headers=data('headers.json');require(headers['headers_checked']>=50 and headers['failures']==0 and all(r['exit_code']==0 for r in headers['results']),'Header checks failed')
 if job['tsan']:
  text=files[commands['crc-tsan']['log']].decode();require(re.search(r'0 failed, [1-9][0-9]* total run',text),'TSan skipped CRC tests');require('WARNING: ThreadSanitizer' not in text,'TSan race');return job
 discovered=data('discovery.json');require(len(discovered['tests'])>=22,'Missing test discovery');names={x['name'] for x in discovered['tests']};require({'native_campaign','sql_campaign','repair_regression_tests','crc_concurrency_tests','all_tests'}<=names,'Required test missing')
 tree=ET.fromstring(files['ctest.xml']);cases=tree.findall('testcase');require(len(cases)==len(discovered['tests']) and all(c.find('failure') is None and c.find('skipped') is None and c.attrib.get('status','run')=='run' for c in cases),'Failed or skipped CTest entry')
 text=files[commands['ctest']['log']].decode();require('100% tests passed' in text,'CTest raw result not successful')
 for n in ('native-campaign.json','native-full.json'):
  native=data(n);require(native['programs_requested']>=10000 and native['programs_compiled']==native['programs_requested'] and native['native_invocations']>=1000000 and native['native_invocations']==native['inputs_evaluated'] and native['failures']==0 and native['spill_programs']>0,'Native campaign missing progress')
  expected='aarch64' if job['machine'] in ('arm64','aarch64') else 'x86_64';require(native['arch']==expected,'Wrong native architecture')
 for n in ('sql-campaign.json','sql-full.json'):
  sql=data(n);require(sql['queries_checked']>=2000 and sql['queries_checked']==sql['queries_requested'] and sql['failures']==0 and sql['native_invocations']>0 and sql['runtime_errors_checked']>0 and sql['empty_inputs']>0 and sql['warm_cache_checks']>0,'SQL campaign missing progress')
 bench=data('benchmarks.json');raw=data('benchmark-raw.json');require(len(bench['repetitions'])==len(raw['repetitions']) and all(all(rep[k]==raw_rep[k] for k in raw_rep) for rep,raw_rep in zip(bench['repetitions'],raw['repetitions'])),'Benchmark raw mismatch');require(bench['input_rows']>0,'Zero benchmark input')
 fixture_hash=hashlib.sha256(files['ingest-0.vts']).hexdigest();require(fixture_hash==bench['input_vts_sha256'] and hashlib.sha256(files['input.pcap']).hexdigest()==bench['workload_pcap_sha256'],'Changed workload')
 groups={};hashes=set()
 for rep in bench['repetitions']:
  key=(rep['engine'],rep['cache']);groups.setdefault(key,[]).append(rep);require(rep['input_sha256']==fixture_hash and rep['wall_ns']>0 and rep['peak_rss_bytes']>0 and rep['result']['rows_scanned']==bench['input_rows'] and len(rep['result']['rows'])>0,'Zero-progress benchmark');require(rep['result_sha256']==semhash(rep['result']),'Changed result digest');hashes.add(rep['result_sha256'])
  if rep['engine']=='jit':require(rep['native_invocations']>=bench['input_rows'] and rep['fallback_count']==0 and rep['rows_evaluated']>=bench['input_rows'] and (rep['functions_compiled']==0 if rep['cache']=='warm' else rep['functions_compiled']>0),'JIT benchmark skipped native execution')
 require(set(groups)=={('interpreter','cold'),('vector','cold'),('jit','cold'),('jit','warm')} and len(hashes)==1,'Benchmark workloads do not agree');require(all(len(x)>=(1 if job['sanitizers'] else 5) for x in groups.values()),'Missing repetitions')
 replay=bench['replay_repetitions'];require(len(replay)>=1 and all(r['delivered']==bench['input_rows'] and r['rows_scanned']==bench['input_rows'] and r['wall_ns']>0 and r['peak_rss_bytes']>0 for r in replay) and len({r['output_digest'] for r in replay})==1,'Replay benchmark missing delivery')
 ingestion=bench['ingestion_repetitions'];require(len(ingestion)>=(1 if job['sanitizers'] else 5) and len({r['result_sha256'] for r in ingestion})==1 and all(r['rows_scanned']==bench['input_rows'] for r in ingestion),'Ingestion mismatch')
 for r in ingestion:
  observed=commands[r['name']];require(r['wall_ns']==observed['wall_ns'] and r['exit_code']==0,'Changed ingestion timing');index=r['name'].split('-')[-1];payload=json.loads(files['ingestion-result-'+index+'.log'].decode().split('\n',1)[1]);require(r['result_sha256']==semhash(semantic(payload)) and len(payload['rows'])==bench['input_rows'],'Ingestion raw digest mismatch')
 cli=bench['cli_agreement'];require(len(cli)==18,'Missing CLI comparisons');digests={}
 for entry in cli:
  result=data(entry['file']);require(entry['semantic_digest']==semhash(semantic(result)),'Changed CLI result');target=entry['file'].rsplit('-',1)[0];digests.setdefault(target,set()).add(entry['semantic_digest']);input_name='demo.vts' if entry['file'].startswith('demo.vts') else 'ingest-0.vts';require(entry['input_sha256']==hashlib.sha256(files[input_name]).hexdigest(),'Changed CLI input')
  if result['engine']=='jit':require(result['observability']['native_invocations']>=result['rows_scanned'] and result['observability']['fallback_count']==0,'CLI JIT skipped native code')
 require(all(len(x)==1 for x in digests.values()),'CLI engines disagree');return job

def verify(out,check_source=True):
 acc=json.loads((out/'ACCEPTANCE.json').read_text());require(acc['status']=='PASSED','Acceptance not passed');manifest=json.loads((out/'SOURCE_MANIFEST.json').read_text());revision=acc['implementation_revision']
 if check_source:
  require(source_manifest()==manifest,'Stale implementation source')
  for n,h in manifest.items():
   blob=subprocess.check_output(['git','show',revision+':'+n],cwd=ROOT);require(hashlib.sha256(blob).hexdigest()==h,'Implementation revision does not match source')
 for n,h in acc['artifact_manifest'].items():require((out/n).is_file() and digest(out/n)==h,'Missing or changed bundle file: '+n)
 ci=json.loads((out/'HOSTED_CI.json').read_text());require(ci['headSha']==revision and ci['conclusion']=='success' and ci['status']=='completed','Stale or failed hosted CI');hosted={j['name']:j for j in ci['jobs']};require(set(REQUIRED)<=set(hosted) and all(hosted[n]['conclusion']=='success' and hosted[n]['status']=='completed' for n in REQUIRED),'Required hosted job failed')
 jobs=[]
 for archive in sorted((out/'jobs').glob('*.tar.gz')):jobs.append(validate_job(load_archive(archive),manifest,revision))
 require({j['job_name'] for j in jobs}>=set(REQUIRED)|{'local-aarch64-release','local-aarch64-sanitizers'},'Missing required raw job archives')
 require({json.loads(load_archive(archive)['native-full.json'])['arch'] for archive in (out/'jobs').glob('*.tar.gz') if 'native-full.json' in load_archive(archive)}=={'x86_64','aarch64'},'Missing native architecture')
 docs=json.loads((out/'DOCUMENT_BYTES.json').read_text());start=docs['starting_revision']
 for n,h in docs['files'].items():
  require(digest(ROOT/n)==h and hashlib.sha256(subprocess.check_output(['git','show',start+':'+n],cwd=ROOT)).hexdigest()==h,'Changed documentation: '+n)
 require({n for n in subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines() if n.lower().split('/')[-1].startswith('readme') or pathlib.Path(n).suffix in ('.md','.rst') or n.startswith('docs/')}==set(docs['files']),'Documentation file set changed')
 return acc

def assemble(out,inputs,hosted_path):
 out.mkdir(parents=True,exist_ok=True);(out/'jobs').mkdir(exist_ok=True);manifest=source_manifest();revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip();ci=json.loads(hosted_path.read_text());require(ci['headSha']==revision,'Hosted revision mismatch')
 jobs=[]
 for directory in inputs:
  for record in directory.rglob('job.json'):
   base=record.parent;files={str(x.relative_to(base)):x.read_bytes() for x in base.rglob('*') if x.is_file()};job=validate_job(files,manifest,revision);jobs.append(job)
   with tarfile.open(out/'jobs'/(job['job_name']+'.tar.gz'),'w:gz') as t:
    for n in sorted(files):info=tarfile.TarInfo(n);info.size=len(files[n]);info.mtime=0;t.addfile(info,io.BytesIO(files[n]))
 dump(out/'SOURCE_MANIFEST.json',manifest);dump(out/'HOSTED_CI.json',ci)
 names=subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines();doc_files={n:digest(ROOT/n) for n in names if n.lower().split('/')[-1].startswith('readme') or pathlib.Path(n).suffix in ('.md','.rst') or n.startswith('docs/')};dump(out/'DOCUMENT_BYTES.json',{'starting_revision':'976fc49d9dda2c788b149910f96b8eaad0a210cd','files':doc_files})
 acc={'schema':2,'status':'PASSED','implementation_revision':revision,'jobs':[j['job_name'] for j in jobs],'artifact_manifest':{str(p.relative_to(out)):digest(p) for p in out.rglob('*') if p.is_file() and p.name not in ('ACCEPTANCE.json','NEGATIVE_TESTS.json')}};dump(out/'ACCEPTANCE.json',acc);verify(out)

def negatives(out):
 acc=verify(out);results=[]
 with tempfile.TemporaryDirectory() as tmp:
  base=pathlib.Path(tmp)/'bundle';shutil.copytree(out,base)
  target=next((base/'jobs').glob('*.tar.gz'));raw=target.read_bytes();target.unlink()
  try:verify(base);raise RuntimeError('Deletion accepted')
  except ValueError as e:results.append({'case':'delete_required_raw','rejected':True,'reason':str(e)})
  target.write_bytes(raw);target.write_bytes(raw[:-1]+bytes([raw[-1]^1]))
  try:verify(base);raise RuntimeError('Altered bytes accepted')
  except ValueError as e:results.append({'case':'alter_raw_bytes','rejected':True,'reason':str(e)})
  target.write_bytes(raw)
  source=ROOT/'src/common/crc32c.cpp';original=source.read_bytes()
  try:
   source.write_bytes(original+b'\n// evidence negative test\n')
   try:verify(base);raise RuntimeError('Modified source accepted')
   except ValueError as e:results.append({'case':'modify_source','rejected':True,'reason':str(e)})
  finally:source.write_bytes(original)
  target=next(p for p in (base/'jobs').glob('*.tar.gz') if 'native-full.json' in load_archive(p));files=load_archive(target);data=json.loads(files['native-full.json']);data['native_invocations']=0;files['native-full.json']=(json.dumps(data)+'\n').encode();job=json.loads(files['job.json']);job['artifact_manifest']['native-full.json']=hashlib.sha256(files['native-full.json']).hexdigest();files['job.json']=(json.dumps(job)+'\n').encode()
  with tarfile.open(target,'w:gz') as t:
   for n,blob in files.items():info=tarfile.TarInfo(n);info.size=len(blob);t.addfile(info,io.BytesIO(blob))
  changed=json.loads((base/'ACCEPTANCE.json').read_text());changed['artifact_manifest'][str(target.relative_to(base))]=digest(target);dump(base/'ACCEPTANCE.json',changed)
  try:verify(base);raise RuntimeError('Zero native count accepted')
  except ValueError as e:results.append({'case':'zero_native_count_with_updated_hashes','rejected':True,'reason':str(e)})
 dump(out/'NEGATIVE_TESTS.json',results);print('All four evidence attacks rejected')
def main():
 p=argparse.ArgumentParser();p.add_argument('--verify-only',action='store_true');p.add_argument('--assemble',action='store_true');p.add_argument('--negative-tests',action='store_true');p.add_argument('--output',default='results/verified/bundle');p.add_argument('--input',action='append',default=[]);p.add_argument('--hosted');a=p.parse_args();out=(ROOT/a.output).resolve()
 try:
  if a.assemble:assemble(out,[(ROOT/n).resolve() for n in a.input],pathlib.Path(a.hosted))
  if a.negative_tests:negatives(out)
  verify(out);print('Evidence verified: '+str(out));return 0
 except (ValueError,KeyError,FileNotFoundError,json.JSONDecodeError,tarfile.TarError,ET.ParseError) as e:print('Evidence rejected: '+str(e),file=sys.stderr);return 1
if __name__=='__main__':sys.exit(main())
