// JS リファレンス: モック app.js と同一の sampleCurve / 制御で CSV を吐く
function sampleCurve(pts, x) {
  const n = pts.length;
  if (!n) return 0;
  if (x <= pts[0].x) return pts[0].y;
  if (x >= pts[n - 1].x) return pts[n - 1].y;
  let i = 0;
  while (i < n - 1 && !(x >= pts[i].x && x <= pts[i + 1].x)) i++;
  const p1 = pts[i], p2 = pts[i + 1];
  const p0 = pts[i - 1] || p1, p3 = pts[i + 2] || p2;
  const span = (p2.x - p1.x) || 1e-6, t = (x - p1.x) / span;
  const t2 = t * t, t3 = t2 * t;
  return 0.5 * ((2 * p1.y) + (-p0.y + p2.y) * t
    + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2
    + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
}
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
const byte = (pts, m) => clamp(Math.round(clamp(sampleCurve(pts, m), 0, 100) / 100 * 255), 0, 255);

const R=[{x:0,y:0},{x:360,y:24},{x:540,y:47},{x:1020,y:47},{x:1140,y:47},{x:1260,y:0},{x:1440,y:0}];
const G=[{x:0,y:0},{x:360,y:16},{x:540,y:55},{x:1020,y:55},{x:1140,y:12},{x:1260,y:0},{x:1440,y:0}];
const B=[{x:0,y:0},{x:360,y:4},{x:540,y:71},{x:1020,y:71},{x:1140,y:4},{x:1260,y:0},{x:1440,y:0}];
const W=[{x:0,y:3},{x:300,y:3},{x:360,y:8},{x:540,y:24},{x:1020,y:24},{x:1140,y:4},{x:1260,y:3},{x:1440,y:3}];
const F=[{x:24,y:0},{x:26,y:0},{x:27,y:40},{x:28.5,y:70},{x:30,y:100},{x:35,y:100}];

const out=[];
for(let m=0;m<=1440;m++){
  out.push(`L,${m},${sampleCurve(R,m).toFixed(6)},${sampleCurve(G,m).toFixed(6)},${sampleCurve(B,m).toFixed(6)},${sampleCurve(W,m).toFixed(6)},${byte(R,m)},${byte(G,m)},${byte(B,m)},${byte(W,m)}`);
}
const FAN_RPM_MAX=2200, FAN_AIRFLOW_K=0.013, FAN_RPM_MIN_ON=350;
const rpmFromDuty = d => d<1?0:Math.max(FAN_RPM_MIN_ON, Math.round(FAN_RPM_MAX*d/100));
for(let k=0;k<=260;k++){
  const t=22.0+k*0.05;
  const d=clamp(sampleCurve(F,t),0,100);
  const rpm=rpmFromDuty(d);
  out.push(`F,${t.toFixed(2)},${d.toFixed(6)},${rpm},${(rpm*FAN_AIRFLOW_K).toFixed(4)}`);
}
const capTarget = t => t<18?18:(t>35?35:t);
const decide = (w,target,hyst,prev)=>{ target=capTarget(target); if(hyst<0)hyst=0; return prev?(w<target):(w<target-hyst); };
{
  let on=false;
  for(let k=0;k<=120;k++){ const w=22.0+k*0.05; on=decide(w,25,0.3,on); out.push(`H,up,${w.toFixed(2)},${on?1:0}`); }
  for(let k=120;k>=0;k--){ const w=22.0+k*0.05; on=decide(w,25,0.3,on); out.push(`H,dn,${w.toFixed(2)},${on?1:0}`); }
  out.push(`CAP,${capTarget(40).toFixed(2)}`);
  out.push(`CAP,${capTarget(10).toFixed(2)}`);
}
process.stdout.write(out.join("\n")+"\n");
