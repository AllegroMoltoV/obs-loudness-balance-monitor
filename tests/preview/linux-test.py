import array, json, pathlib, subprocess, sys, time
exe=str(pathlib.Path(sys.argv[1]).resolve())
results=[]
observe="--observe" in sys.argv
label="baseline-" if observe else ""
for rate in (44100,48000):
 sink="lbm_probe_"+str(rate)
 module=subprocess.check_output(["pactl","load-module","module-null-sink","sink_name="+sink,"format=float32le","rate="+str(rate),"channels=2"],text=True).strip()
 pcm=array.array("f",(float((i%8191)+1)/4194304 for i in range(rate*5*2)))
 data=pcm.tobytes();path=pathlib.Path("input-"+str(rate)+".f32");path.write_bytes(data)
 try:
  for mode in ("natural","cancel"):
   out=pathlib.Path(label+str(rate)+"-"+mode+".f32")
   with out.open("wb") as f:
    capture=subprocess.Popen(["parec","--raw","--format=float32le","--rate="+str(rate),"--channels=2","--device="+sink+".monitor"],stdout=f,stderr=subprocess.PIPE)
    try:
     deadline=time.monotonic()+10
     while out.stat().st_size < 8192:
      assert capture.poll() is None, "capture exited before ready"
      assert time.monotonic() < deadline, "capture did not deliver initial audio"
      time.sleep(.01)
     args=[exe,"--play",str(path),str(rate),sink]+(["1000"] if mode=="cancel" else [])
     played=subprocess.run(args,capture_output=True,text=True,timeout=25)
     print(played.stdout,played.stderr,flush=True)
     time.sleep(.25)
    finally:
     capture.terminate();_,err=capture.communicate(timeout=5)
   audio=out.read_bytes()
   expected_code=0 if mode=="natural" else 3
   record={"rate":rate,"mode":mode,"returncode":played.returncode,"capture_bytes":len(audio),"capture_stderr":err.decode(errors="replace")}
   if mode=="natural":
    record["full_pcm_offset"]=audio.find(data)
    record["pcm_ok"]=record["full_pcm_offset"]>=0 and record["full_pcm_offset"]%8==0
   else:
    record["full_clip_present"]=data in audio
    record["signal_prefix_present"]=data[:8192] in audio
    record["pcm_ok"]=not record["full_clip_present"] and record["signal_prefix_present"]
   record["passed"]=record["pcm_ok"] and played.returncode==expected_code
   results.append(record)
   pathlib.Path(label+"linux-results.json").write_text(json.dumps(results,indent=2))
   print(json.dumps(record),flush=True)
   if not observe: assert record["passed"],record
  invalid=subprocess.run([exe,"--play",str(path),str(rate),"lbm_missing_sink"],capture_output=True,text=True,timeout=15)
  assert invalid.returncode==2,invalid.stdout
 finally:
  subprocess.run(["pactl","unload-module",module],check=True)
pathlib.Path(label+"linux-results.json").write_text(json.dumps(results,indent=2))
print(json.dumps(results,indent=2))
