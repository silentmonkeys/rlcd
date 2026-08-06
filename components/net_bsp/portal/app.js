var LIM={marks:32,events:16,labels:16,text:31},dirty=false,curSsid='';
var marks=[],events=[],labels=[],rawRows=null,chart=null;
function $(i){return document.getElementById(i)}
function on(i,f){var e=$(i);if(e)e.onclick=f}
var tt;function toast(m,k){var t=$('toast');t.textContent=m;
t.className='show'+(k?' '+k:'');clearTimeout(tt);
tt=setTimeout(function(){t.className=''},k==='bad'?4200:2200)}
function api(u,o){return fetch(u,o).then(function(r){
return r.text().then(function(t){var j={};try{j=JSON.parse(t)}catch(e){}
if(!r.ok||j.ok===false)throw new Error(j.err||('HTTP '+r.status));return j})})}
function post(u,o){return api(u,{method:'POST',
headers:{'Content-Type':'application/json'},body:JSON.stringify(o||{})})}
function busy(id,b){var e=$(id);if(e)e.disabled=b}
function pwbtn(btnId,inputId){
  on(btnId,function(){
    var inp=$(inputId),btn=$(btnId);
    if(inp.type==='password'){inp.type='text';btn.textContent='👁';btn.className='pwbtn show'}
    else{inp.type='password';btn.textContent='•';btn.className='pwbtn hide'}
  })
}
function toggleCard(id){
  var btn=$(id+'_btn');
  var card=btn.closest('.card');
  var collapsed=card.classList.toggle('collapsed');
  btn.textContent=collapsed?'展开 ›':'收起';
  localStorage.setItem('cal_'+id,collapsed?'1':'0');
}
function esc(s){return (''+s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
.replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;')}
function dur(s){if(s==null)return '—';var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
m=Math.floor(s%3600/60);return d?d+'天 '+h+'小时':(h?h+'小时 '+m+'分':m+'分')}
function bars(r){return r>-55?'▂▄▆█':r>-68?'▂▄▆':r>-78?'▂▄':'•'}
function badText(t){if(!t)return '内容不能为空';
if(/[;=,\r\n]/.test(t))return '不能包含 ; = , 或换行';
if(new Blob([t]).size>LIM.text)return '太长了（最多约 10 个汉字）';return ''}
function mmdd(v){return v?v.slice(5):''}
function tick(){api('/api/status').then(function(s){
  $('hdrdot').className='dot '+(s.wifi?'on':'off');
  $('hdrnet').textContent=s.wifi?(s.ssid||'已连接')+' · '+s.ip:'未连接 WiFi';
  curSsid=s.ssid||'';
  var S=[];
  S.push({k:'WiFi',v:s.wifi?'已连接':'未连接'});S.push({k:'信号',v:s.rssi+' dBm'});
  S.push({k:'IP 地址',v:s.ip||'—'});S.push({k:'室内温度',v:s.temp!=null?s.temp.toFixed(1)+' ℃':'—'});
  S.push({k:'室内湿度',v:s.humi!=null?s.humi.toFixed(0)+' %':'—'});
  S.push({k:'电池',v:s.batt!=null?s.batt+'%'+(s.charging?' ⚡':''):'—'});
  S.push({k:'室外',v:(s.otemp!=null?s.otemp.toFixed(1)+' ℃':'—')+(s.wtext?' '+s.wtext:'')});
  S.push({k:'天气更新',v:s.wupd||'尚未获取'});S.push({k:'SD 卡',v:s.sd?(s.sd_used+' / '+s.sd_total+' MB'):'未插入'});
  S.push({k:'运行时长',v:dur(s.uptime)});
  $('stat').innerHTML=S.map(function(x){return '<div class=kv><div class=k>'+esc(x.k)+'</div><div class=v>'+esc(x.v)+'</div></div>'}).join('');
  var W=[];W.push({k:'城市',v:s.city||'—'});W.push({k:'天气',v:s.wtext||'—'});
  W.push({k:'室外温度',v:s.otemp!=null?s.otemp.toFixed(1)+' ℃':'—'});
  W.push({k:'体感温度',v:s.feels!=null?s.feels.toFixed(1)+' ℃':'—'});
  W.push({k:'室外湿度',v:s.ohumi!=null?s.ohumi.toFixed(0)+' %':'—'});W.push({k:'风速',v:s.wind||'—'});
  W.push({k:'今日范围',v:s.tmin!=null?s.tmin+' ~ '+s.tmax+' ℃':'—'});W.push({k:'最后更新',v:s.wupd||'—'});
  $('wxnow').innerHTML=W.map(function(x){return '<div class=kv><div class=k>'+esc(x.k)+'</div><div class=v>'+esc(x.v)+'</div></div>'}).join('');
  var Y=[];Y.push({k:'型号',v:s.chip||'—'});Y.push({k:'固件',v:s.app||'—'});Y.push({k:'ESP-IDF',v:s.idf||'—'});
  Y.push({k:'MAC',v:s.mac||'—'});Y.push({k:'空闲内存',v:s.heap!=null?s.heap+' KB':'—'});
  Y.push({k:'Flash 剩余',v:s.flash_free!=null?s.flash_free+' KB':'—'});Y.push({k:'运行时长',v:dur(s.uptime)});
  Y.push({k:'SD 卡',v:s.sd?'已挂载':'未插入'});
  $('sysinfo').innerHTML=Y.map(function(x){return '<div class=kv><div class=k>'+esc(x.k)+'</div><div class=v>'+esc(x.v)+'</div></div>'}).join('');
  if(!s.sd){$('nosd').hidden=false;$('calbody').classList.add('off');}
  else{$('nosd').hidden=true;$('calbody').classList.remove('off');}
}).catch(function(){})}

function cnt(id,n,max){$(id).textContent=n+' / '+max}
function render(){
  function box(id,arr,f){$(id).innerHTML=arr.map(function(v,i){
    return '<div class=it><span class=tx>'+esc(f(v))+'</span>'
      +'<button type=button class=x data-l='+id+' data-i='+i+'>×</button></div>'}).join('');
    $(id).querySelectorAll('.x').forEach(function(e){e.onclick=function(){
      ({marks:marks,events:events,labels:labels})[e.dataset.l].splice(+e.dataset.i,1);
      dirty=true;render()}})}
  box('marks',marks,function(v){return v});box('events',events,function(v){return v.date+' → '+v.text});
  box('labels',labels,function(v){return v});cnt('c_marks',marks.length,LIM.marks);
  cnt('c_events',events.length,LIM.events);cnt('c_labels',labels.length,LIM.labels);
  busy('btnmark',marks.length>=LIM.marks);busy('btnevt',events.length>=LIM.events);
  busy('btnlab',labels.length>=LIM.labels)}

// 只留 5 个时间选项对应的点数
var MAX_DISPLAY=500;
function parseCsvStream(text){
  var lines=text.split(/\r?\n/);lines.shift();
  // 倒着读，取最新 N 行，时间最旧的在前
  var rows=[],step=Math.max(1,Math.ceil(lines.length/MAX_DISPLAY));
  for(var i=Math.max(0,lines.length-MAX_DISPLAY*step);i<lines.length;i+=step){
    if(!lines[i])continue;
    var cols=[],s='',q=false,line=lines[i];
    for(var j=0;j<line.length;j++){
      if(line[j]=='"')q=!q;
      else if(line[j]==','&&!q){cols.push(s);s=''}
      else s+=line[j];
    }cols.push(s);if(cols.length>=3)rows.push({
      t:cols[0],it:parseFloat(cols[1]),ih:parseFloat(cols[2])
    });
  }rawRows=rows;toast('最新 '+rows.length+' 点数据','ok');drawChart();
}

function drawChart(){
  if(!rawRows||!rawRows.length||!window.Chart)return;
  var ctx=$('cv').getContext('2d');if(chart)chart.destroy();
  var n=parseInt($('range_sel').value,10);
  var d=rawRows.slice(-Math.min(rawRows.length,n));
  var isDark=window.matchMedia('(prefers-color-scheme: dark)').matches;
  chart=new Chart(ctx,{
    type:'line',data:{
      labels:d.map(function(x){return x.t.slice(5,16).replace(' ','\n')}),
      datasets:[{
        label:'室内温度 (℃)',data:d.map(function(x){return x.it}),
        borderColor:'#2563eb',backgroundColor:'rgba(37,99,235,.1)',
        tension:.25,borderWidth:2,pointRadius:0,pointHoverRadius:4,fill:true,yAxisID:'y'
      },{
        label:'室内湿度 (%)',data:d.map(function(x){return x.ih}),
        borderColor:'#059669',backgroundColor:'rgba(5,150,105,.1)',
        tension:.25,borderWidth:2,pointRadius:0,pointHoverRadius:4,fill:true,yAxisID:'y1'
      }]
    },options:{
      responsive:true,maintainAspectRatio:false,interaction:{mode:'index',intersect:false},
      plugins:{
        legend:{position:'bottom',labels:{font:{size:12},usePointStyle:true,
          color:isDark?'#e8eaed':'#16181d',boxWidth:12}},
        tooltip:{mode:'index',intersect:false,backgroundColor:isDark?'rgba(0,0,0,.85)':'rgba(0,0,0,.8)',
          padding:8,cornerRadius:6,titleFont:{size:12},bodyFont:{size:12}}
      },
      scales:{
        x:{grid:{display:false},ticks:{font:{size:10},color:isDark?'#9499a3':'#9499a3',
          maxTicksLimit:7,maxRotation:0}},
        y:{position:'left',grid:{color:isDark?'rgba(255,255,255,.06)':'rgba(0,0,0,.05)'},
          ticks:{font:{size:11},color:'#2563eb'},title:{display:true,text:'温度 ℃',
          color:'#2563eb',font:{size:11}}},
        y1:{position:'right',grid:{display:false},
          ticks:{font:{size:11},color:'#059669'},title:{display:true,text:'湿度 %',
          color:'#059669',font:{size:11}}}
      }
    }
  });
}
// Chart.js（gzip 后约 70KB）按需加载：只有真正打开"数据"页才去下。
// 首屏因此不必等这 70KB —— 对 ESP32-S3 的 5760B TCP 窗口差别很明显。
var chartLib=null;
function ensureChart(){
  if(chartLib)return chartLib;
  chartLib=new Promise(function(res,rej){
    var s=document.createElement('script');
    s.src=CHART_URL;
    s.onload=function(){res(window.Chart)};
    s.onerror=function(){chartLib=null;rej(new Error('图表库加载失败'))};
    document.head.appendChild(s);
  });
  return chartLib;
}
function loadData(){
  return ensureChart().then(function(){
    if(rawRows)return drawChart();
    return fetch('/api/data/csv')
      .then(function(r){if(!r.ok)throw new Error('数据获取失败');return r.text()})
      .then(function(t){parseCsvStream(t)});
  }).catch(function(e){toast(e.message,'bad')});
}

document.addEventListener('DOMContentLoaded', function(){
  pwbtn('p_toggle','f_pass');pwbtn('h_toggle','f_host');pwbtn('k_toggle','f_key');
  ['marks','events','labels'].forEach(function(id){
    var collapsed=localStorage.getItem('cal_'+id)!=='0';
    var btn=$(id+'_btn'),card=btn.closest('.card');
    if(collapsed){card.classList.add('collapsed');btn.textContent='展开 ›'}
    else btn.textContent='收起';
  });
  document.querySelectorAll('.tab').forEach(function(b){b.onclick=function(){
    var n=b.dataset.t;document.querySelectorAll('.tab').forEach(function(x,i){
      x.classList.toggle('active',''+i==n)});
    document.querySelectorAll('.page').forEach(function(x,i){
      x.classList.toggle('active',''+i==n)});if(n==4)setTimeout(loadData,100);
  }});
  on('btnscan',function(){var b=$('scan');b.innerHTML='<div class=msg>扫描中…</div>';
  busy('btnscan',true);api('/api/scan').then(function(j){var l=j.aps||[];
    if(!l.length){b.innerHTML='<div class=msg>未发现 2.4GHz 网络</div>';return}
    b.innerHTML=l.map(function(a){return '<button type=button class=ap data-s='+
      esc(a.ssid).replace(/'/g,'&#39;')+'>'+
      '<span class=nm>'+(a.auth?'🔒 ':'')+esc(a.ssid)+'</span>'+
      (a.ssid==curSsid?'<span class=cur>当前</span>':'')+
      '<span class=rs>'+bars(a.rssi)+' '+a.rssi+'</span></button>'}).join('');
    b.querySelectorAll('.ap').forEach(function(e){e.onclick=function(){
      $('f_ssid').value=e.dataset.s;toast('已选择 '+e.dataset.s)}})
  }).catch(function(e){b.innerHTML='<div class=msg err>扫描失败：'+esc(e.message)+'</div>'})
  .then(function(){busy('btnscan',false)})});
  on('btnnet',function(){var s=$('f_ssid').value.trim();if(!s){toast('请填写 WiFi 名称','bad');return}
    if(!confirm('保存后设备会立即重启并尝试连接 '+s+'。继续？'))return;busy('btnnet',true);
    post('/api/config',{ssid:s,pass:$('f_pass').value}).then(function(){
      document.body.innerHTML='<div class=msg style=padding:60px>已保存，设备正在重启…</div>'
    }).catch(function(e){toast(e.message,'bad');busy('btnnet',false)})});
  on('btnwxsave',function(){busy('btnwxsave',true);
    post('/api/config',{city:$('f_city').value.trim(),host:$('f_host').value,
apikey:$('f_key').value}).then(function(){
      $('f_key').value='';$('f_host').value='';toast('已保存，正在刷新天气','ok');
      return post('/api/weather_refresh')}).catch(function(e){toast(e.message,'bad')})
    .then(function(){busy('btnwxsave',false);setTimeout(tick,3000)})});
  on('btnwx',function(){busy('btnwx',true);post('/api/weather_refresh')
    .then(function(){toast('已触发刷新','ok');setTimeout(tick,3000)})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btnwx',false)})});
  on('btnmark',function(){var v=mmdd($('i_mark').value);if(!v){toast('请先选择日期','bad');return}
    if(marks.indexOf(v)>=0){toast('该日期已在列表中','bad');return}
    marks.push(v);marks.sort();$('i_mark').value='';dirty=true;render()});
  on('btnevt',function(){var d=mmdd($('i_edate').value),t=$('i_etext').value.trim();
    if(!d){toast('请先选择日期','bad');return}var e=badText(t);if(e){toast(e,'bad');return}
    for(var i=0;i<events.length;i++)if(events[i].date==d){events[i].text=t;
      $('i_edate').value='';$('i_etext').value='';dirty=true;render();
      toast('已更新 '+d+' 的预定');return}
    events.push({date:d,text:t});events.sort(function(a,b){return a.date<b.date?-1:1});
    $('i_edate').value='';$('i_etext').value='';dirty=true;render()});
  on('btnlab',function(){var t=$('i_label').value.trim();var e=badText(t);if(e){toast(e,'bad');return}
    if(labels.indexOf(t)>=0){toast('该标签已存在','bad');return}
    labels.push(t);$('i_label').value='';dirty=true;render()});
  $('lab_on').onchange=function(){$('labbox').hidden=!this.checked;dirty=true};
  on('btncal',function(){busy('btncal',true);
    post('/api/calendar',{marks:marks,events:events,labels:$('lab_on').checked?labels:[]})
    .then(function(){dirty=false;toast('已写入 SD 卡','ok')})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btncal',false)})});
  $('range_sel').onchange=drawChart;
  on('btndl',function(){busy('btndl',true);fetch('/api/data/csv')
    .then(function(r){if(!r.ok)throw new Error('下载失败');return r.blob()})
    .then(function(b){var a=document.createElement('a');a.href=URL.createObjectURL(b);
      a.download='rlcd_weather_'+(new Date().toISOString().slice(0,10))+'.csv';
      a.click();toast('下载完成','ok')})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btndl',false)})});
  on('btnreboot',function(){if(!confirm('确定要重启设备？'))return;
    post('/api/reboot').catch(function(){});toast('正在重启…')});
  on('btnforget',function(){if(!confirm('将清除已保存的 WiFi 配置并重启，确定？'))return;
    post('/api/forget').catch(function(){});toast('已清除，正在重启…')});
  api('/api/limits').then(function(l){LIM=l;render()}).catch(function(){render()});
  api('/api/calendar').then(function(c){marks=c.marks||[];events=c.events||[];labels=c.labels||[];
    $('lab_on').checked=labels.length>0;$('labbox').hidden=!labels.length;render()}).catch(function(){render()});
  api('/api/config').then(function(c){
    $('f_ssid').value=c.ssid||'';$('f_city').value=c.city||'';
    $('f_pass').placeholder=c.has_pass?'••••••••':'尚未设置';
    $('f_key').placeholder=c.has_key?'••••••••':'尚未设置';
    $('f_host').placeholder=c.has_host?'••••••••':'xxx.re.qweatherapi.com';
  }).catch(function(){});
  tick();setInterval(tick,5000);
  setTimeout(function(){var obs=new IntersectionObserver(function(es){es.forEach(function(e){
    if(e.isIntersecting&&!rawRows){obs.disconnect();loadData();}
  })});obs.observe($('cv'));},200);
  window.onbeforeunload=function(){if(dirty)return '日历有未保存的修改'};
});
