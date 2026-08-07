var LIM={marks:32,events:16,labels:16,text:31},dirty=false,curSsid='';
var marks=[],events=[],labels=[],rawRows=null,chart=null;
// 5 秒状态轮询的句柄 —— OTA 上传期间要停掉它（见 btnota）
var tickTimer=null;
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
// 明文/密文切换：只翻 class，图标由 CSS 的 .pwbtn.show/.hide mask 画，
// 这里不再往 textContent 塞字符（emoji 在各平台字形和字号都不受控）
function pwbtn(btnId,inputId){
  on(btnId,function(){
    var inp=$(inputId),btn=$(btnId),showing=inp.type==='password';
    inp.type=showing?'text':'password';
    btn.className='pwbtn '+(showing?'show':'hide');
    btn.setAttribute('aria-label',showing?'隐藏内容':'显示内容');
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
// .grid > .kv 的统一渲染 —— 状态/天气/系统/公网 IP 四处共用同一套结构
function kvHtml(a){return a.map(function(x){return '<div class=kv><div class=k>'
+esc(x.k)+'</div><div class=v>'+esc(x.v)+'</div></div>'}).join('')}
function dur(s){if(s==null)return '—';var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
m=Math.floor(s%3600/60);return d?d+'天 '+h+'小时':(h?h+'小时 '+m+'分':m+'分')}
function bars(r){return r>-55?'▂▄▆█':r>-68?'▂▄▆':r>-78?'▂▄':'•'}
function badText(t){if(!t)return '内容不能为空';
if(/[;=,\r\n]/.test(t))return '内容不能包含分号、等号、逗号或换行符';
if(new Blob([t]).size>LIM.text)return '内容过长，最多约 10 个汉字';return ''}
function mmdd(v){return v?v.slice(5):''}
function tick(){api('/api/status').then(function(s){
  $('hdrdot').className='dot '+(s.wifi?'on':'off');
  $('hdrnet').textContent=s.wifi?(s.ssid||'已连接')+' · '+s.ip:'未连接 WiFi';
  curSsid=s.ssid||'';
  var S=[];
  S.push({k:'WiFi',v:s.wifi?'已连接':'未连接'});S.push({k:'信号',v:s.rssi+' dBm'});
  S.push({k:'IP 地址',v:s.ip||'—'});S.push({k:'室内温度',v:s.temp!=null?s.temp.toFixed(1)+' ℃':'—'});
  S.push({k:'室内湿度',v:s.humi!=null?s.humi.toFixed(0)+' %':'—'});
  S.push({k:'电池',v:s.batt!=null?s.batt+'%'+(s.charging?'（充电中）':''):'—'});
  S.push({k:'室外',v:(s.otemp!=null?s.otemp.toFixed(1)+' ℃':'—')+(s.wtext?' '+s.wtext:'')});
  S.push({k:'天气更新',v:s.wupd||'尚未获取'});S.push({k:'SD 卡',v:s.sd?(s.sd_used+' / '+s.sd_total+' MB'):'未插入'});
  S.push({k:'运行时长',v:dur(s.uptime)});
  $('stat').innerHTML=kvHtml(S);
  var W=[];W.push({k:'城市',v:s.city||'—'});W.push({k:'天气',v:s.wtext||'—'});
  W.push({k:'室外温度',v:s.otemp!=null?s.otemp.toFixed(1)+' ℃':'—'});
  W.push({k:'体感温度',v:s.feels!=null?s.feels.toFixed(1)+' ℃':'—'});
  W.push({k:'室外湿度',v:s.ohumi!=null?s.ohumi.toFixed(0)+' %':'—'});W.push({k:'风速',v:s.wind||'—'});
  W.push({k:'今日范围',v:s.tmin!=null?s.tmin+' ~ '+s.tmax+' ℃':'—'});W.push({k:'最后更新',v:s.wupd||'—'});
  $('wxnow').innerHTML=kvHtml(W);
  var Y=[];Y.push({k:'型号',v:s.chip||'—'});Y.push({k:'固件',v:s.app||'—'});Y.push({k:'ESP-IDF',v:s.idf||'—'});
  Y.push({k:'MAC',v:s.mac||'—'});Y.push({k:'空闲内存',v:s.heap!=null?s.heap+' KB':'—'});
  Y.push({k:'Flash 剩余',v:s.flash_free!=null?s.flash_free+' KB':'—'});Y.push({k:'运行时长',v:dur(s.uptime)});
  Y.push({k:'SD 卡',v:s.sd?'已挂载':'未插入'});
  $('sysinfo').innerHTML=kvHtml(Y);
  if(!s.sd){$('nosd').hidden=false;$('calbody').classList.add('off');}
  else{$('nosd').hidden=true;$('calbody').classList.remove('off');}
}).catch(function(){})}

// 公网 IP（uapis.cn /network/myip）—— 布局与天气卡片一致，同一套 .grid/.kv
// 设备侧一天只拉一次，这里读的是缓存，随时可刷；auto=true 表示城市正由它自动定位。
function pubipRender(p){
  var P=[];
  P.push({k:'公网 IP',v:p&&p.valid?p.ip:'—'});
  P.push({k:'归属地',v:p&&p.valid&&p.region?p.region:'—'});
  P.push({k:'行政区',v:p&&p.valid&&p.district?p.district:'—'});
  P.push({k:'运营商',v:p&&p.valid&&p.isp?p.isp:'—'});
  $('pubip').innerHTML=kvHtml(P);
  var h=$('pubiphint');
  if(!p||!p.valid)h.textContent='尚未获取。设备联网后每天获取一次。';
  else if(p.auto)h.textContent='当前天气城市：'+(p.city||'—')+'（自动定位）';
  else h.textContent='已手动指定城市，自动定位未启用。';
}
function loadPubip(){return api('/api/pubip').then(pubipRender).catch(function(){})}

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
      t:cols[0],it:parseFloat(cols[1]),ih:parseFloat(cols[2]),raw:line
    });
  }rawRows=rows;toast('已载入最新 '+rows.length+' 条记录','ok');drawChart();
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
    s.onerror=function(){chartLib=null;rej(new Error('图表库加载失败，请检查网络连接'))};
    document.head.appendChild(s);
  });
  return chartLib;
}
function loadData(){
  return ensureChart().then(function(){
    if(rawRows)return drawChart();
    return fetch('/api/data/csv')
      .then(function(r){if(!r.ok)throw new Error('数据获取失败，请稍后重试');return r.text()})
      .then(function(t){parseCsvStream(t)});
  }).catch(function(e){toast(e.message,'bad')});
}

document.addEventListener('DOMContentLoaded', function(){
  pwbtn('p_toggle','f_pass');pwbtn('h_toggle','f_host');pwbtn('k_toggle','f_key');
  pwbtn('u_toggle','f_uapi');
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
  on('btnstat',function(){busy('btnstat',true);
    tick();loadPubip();setTimeout(function(){busy('btnstat',false)},600)});
  on('btnscan',function(){var b=$('scan');b.innerHTML='<div class=msg>正在扫描</div>';
  busy('btnscan',true);api('/api/scan').then(function(j){var l=j.aps||[];
    if(!l.length){b.innerHTML='<div class=msg>未发现可用的 2.4GHz 网络</div>';return}
    b.innerHTML=l.map(function(a){return '<button type=button class=ap data-s='+
      esc(a.ssid).replace(/'/g,'&#39;')+'>'+
      '<span class=nm>'+esc(a.ssid)+'</span>'+
      (a.auth?'<span class=lock title=加密网络></span>':'')+
      (a.ssid==curSsid?'<span class=cur>当前</span>':'')+
      '<span class=rs>'+bars(a.rssi)+' '+a.rssi+'</span></button>'}).join('');
    b.querySelectorAll('.ap').forEach(function(e){e.onclick=function(){
      $('f_ssid').value=e.dataset.s;toast('已选择网络 '+e.dataset.s)}})
  }).catch(function(e){b.innerHTML='<div class=msg err>扫描失败：'+esc(e.message)+'</div>'})
  .then(function(){busy('btnscan',false)})});
  on('btnnet',function(){var s=$('f_ssid').value.trim();if(!s){toast('请输入 WiFi 名称','bad');return}
    if(!confirm('保存后设备将立即重启并连接网络 '+s+'。是否继续？'))return;busy('btnnet',true);
    post('/api/config',{ssid:s,pass:$('f_pass').value}).then(function(){
      document.body.innerHTML='<div class=msg style=padding:60px>配置已保存，设备正在重启。</div>'
    }).catch(function(e){toast(e.message,'bad');busy('btnnet',false)})});
  on('btnwxsave',function(){busy('btnwxsave',true);
    // host/apikey/uapikey 是掩码字段（后端只回 has_* 布尔），留空即"不修改"：
    // **不能把空串发上去** —— 后端拿空串覆盖会把 Host/Key 清掉，天气就此不再更新。
    // 城市相反：空串是有效值（= 按公网 IP 自动定位），所以无条件带上。
    var body={city:$('f_city').value.trim()};
    if($('f_host').value)body.host=$('f_host').value;
    if($('f_key').value)body.apikey=$('f_key').value;
    if($('f_uapi').value.trim())body.uapikey=$('f_uapi').value.trim();
    post('/api/config',body).then(function(){
      $('f_key').value='';$('f_host').value='';$('f_uapi').value='';
      toast('配置已保存，正在刷新天气数据','ok');
      return post('/api/weather_refresh')}).catch(function(e){toast(e.message,'bad')})
    .then(function(){busy('btnwxsave',false);setTimeout(function(){tick();loadPubip()},3000)})});
  on('btnwx',function(){busy('btnwx',true);post('/api/weather_refresh')
    .then(function(){toast('已触发天气刷新','ok');setTimeout(tick,3000)})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btnwx',false)})});
  // 重新定位：仅在城市留空时后端才受理（否则回 409，手填城市优先）
  on('btncity',function(){
    if($('f_city').value.trim()){toast('请先清空城市字段，再执行重新定位','bad');return}
    busy('btncity',true);post('/api/city_refresh')
    .then(function(){toast('正在定位，请稍候','ok');
      setTimeout(function(){tick();loadPubip()},5000)})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btncity',false)})});
  // 公网 IP 卡片的刷新：读设备缓存（一天一次的拉取结果），不额外消耗 API 配额
  on('btnpubip',function(){busy('btnpubip',true);
    loadPubip().then(function(){busy('btnpubip',false)})});
  on('btnmark',function(){var v=mmdd($('i_mark').value);if(!v){toast('请先选择日期','bad');return}
    if(marks.indexOf(v)>=0){toast('该日期已存在于列表中','bad');return}
    marks.push(v);marks.sort();$('i_mark').value='';dirty=true;render()});
  on('btnevt',function(){var d=mmdd($('i_edate').value),t=$('i_etext').value.trim();
    if(!d){toast('请先选择日期','bad');return}var e=badText(t);if(e){toast(e,'bad');return}
    for(var i=0;i<events.length;i++)if(events[i].date==d){events[i].text=t;
      $('i_edate').value='';$('i_etext').value='';dirty=true;render();
      toast('已更新 '+d+' 的事项内容');return}
    events.push({date:d,text:t});events.sort(function(a,b){return a.date<b.date?-1:1});
    $('i_edate').value='';$('i_etext').value='';dirty=true;render()});
  on('btnlab',function(){var t=$('i_label').value.trim();var e=badText(t);if(e){toast(e,'bad');return}
    if(labels.indexOf(t)>=0){toast('该标签已存在','bad');return}
    labels.push(t);$('i_label').value='';dirty=true;render()});
  $('lab_on').onchange=function(){$('labbox').hidden=!this.checked;dirty=true};
  on('btncal',function(){busy('btncal',true);
    post('/api/calendar',{marks:marks,events:events,labels:$('lab_on').checked?labels:[]})
    .then(function(){dirty=false;toast('日历已保存至 SD 卡','ok')})
    .catch(function(e){toast(e.message,'bad')}).then(function(){busy('btncal',false)})});
  $('range_sel').onchange=drawChart;
  // ---- 数据导出：两种范围 ------------------------------------------
  // CSV 表头与设备侧 user_app.c 的 CSV_HEADER 保持一致。
  var CSV_HEAD='timestamp,indoor_temp,indoor_humi,outdoor_temp,outdoor_humi,weather,city,wifi_rssi';
  function fsize(n){return n>=1048576?(n/1048576).toFixed(1)+' MB'
    :Math.max(1,Math.round(n/1024))+' KB'}
  function saveBlob(b,tag){
    if(!b.size)throw new Error('导出内容为空');
    var u=URL.createObjectURL(b),a=document.createElement('a');a.href=u;
    a.download='rlcd_'+tag+'_'+(new Date().toISOString().slice(0,10))+'.csv';
    document.body.appendChild(a);a.click();a.remove();
    setTimeout(function(){URL.revokeObjectURL(u)},10000);
    toast('已导出 '+fsize(b.size),'ok');
  }
  // 图表数据：直接用已在内存里的行（含原始 CSV 文本），不再请求设备，
  // 导出范围与图表当前所选时间范围严格一致。
  on('btndlview',function(){
    if(!rawRows||!rawRows.length){toast('图表数据尚未载入，请稍后重试','bad');return}
    var n=parseInt($('range_sel').value,10);
    var d=rawRows.slice(-Math.min(rawRows.length,n));
    try{
      saveBlob(new Blob([CSV_HEAD+'\n'+d.map(function(x){return x.raw}).join('\n')+'\n'],
        {type:'text/csv;charset=utf-8'}),'chart');
    }catch(e){toast(e.message,'bad')}
  });
  // 完整记录：走 ?full=1。不带该参数拿到的是图表用的最新若干行（约 3.5 天），
  // 作为存档会遗漏更早的数据。
  //
  // 全量导出可能持续几秒到几十秒（一年数据约 3.6MB，从 SD 卡边读边发）。设备侧
  // httpd 是**单任务串行**的，这条请求全程独占它，所以先停掉 5 秒状态轮询 ——
  // 否则那些 /api/status 只会堆在 backlog 里（backlog_conn=5），导出结束后一次性
  // 涌入，还可能挤掉连接（max_open_sockets=7 且 lru_purge_enable=false）。
  on('btndl',function(){busy('btndl',true);toast('正在导出完整记录，请稍候');
    if(tickTimer){clearInterval(tickTimer);tickTimer=null}
    fetch('/api/data/csv?full=1')
    .then(function(r){if(!r.ok)throw new Error('导出失败（HTTP '+r.status+'）');return r.blob()})
    .then(function(b){saveBlob(b,'full')})
    .catch(function(e){toast(e.message,'bad')}).then(function(){
      busy('btndl',false);
      if(!tickTimer){tick();tickTimer=setInterval(tick,5000)}
    })});
  // ---- 固件升级（OTA） ---------------------------------------------
  // 用 XHR 而不是 fetch：只有 XHR 能报告**上传**进度（fetch 的 ReadableStream
  // 上传在多数浏览器仍不可用）。固件以 raw body 发送，设备边收边写 Flash。
  $('f_fw').onchange=function(){
    var f=this.files&&this.files[0];
    busy('btnota',!f);
    if(f)$('otamsg').textContent='待上传文件：'+f.name+'（'+fsize(f.size)+'）';
    $('otabox').hidden=!f;
    $('otabar').style.width='0';$('otabar').parentNode.className='bar';
  };
  on('btnota',function(){
    var f=$('f_fw').files&&$('f_fw').files[0];
    if(!f){toast('请先选择固件文件','bad');return}
    if(!/\.bin$/i.test(f.name)){toast('固件文件应为 .bin 格式','bad');return}
    if(!confirm('将向设备写入固件 '+f.name+'，完成后设备自动重启。是否继续？'))return;
    busy('btnota',true);$('f_fw').disabled=true;$('otabox').hidden=false;
    var bar=$('otabar'),wrap=bar.parentNode,msg=$('otamsg');
    wrap.className='bar';bar.style.width='0';msg.textContent='正在上传 0%';
    // 上传期间停掉 5 秒状态轮询：设备侧 httpd 是**单任务串行**处理，ota_post
    // 全程独占它，这些请求只会堆在 backlog 里（backlog_conn=5），上传结束后
    // 一次性涌入。同理，上传中也**无法**去查设备侧写入进度 —— 那个请求同样
    // 排不上队（这就是 /api/ota_status 被删掉的原因）。
    if(tickTimer){clearInterval(tickTimer);tickTimer=null}
    function resumeTick(){if(!tickTimer)tickTimer=setInterval(tick,5000)}
    var x=new XMLHttpRequest();
    x.open('POST','/api/ota',true);
    x.setRequestHeader('Content-Type','application/octet-stream');
    x.timeout=300000;   // 大固件 + 弱信号，给足 5 分钟
    x.upload.onprogress=function(e){
      if(!e.lengthComputable)return;
      var p=Math.round(e.loaded*100/e.total);
      bar.style.width=p+'%';
      // 到 100% 只代表**发完了**（socket 缓冲会让它提前到达），设备还在写
      // 最后几块 + 校验 SHA256 + 切分区，这段时间没有进度可查，只能给文案。
      msg.textContent=p<100?'正在上传 '+p+'%':'上传完成，设备正在校验并写入，请勿断电';
    };
    function fail(m){resumeTick();
      wrap.className='bar bad';msg.textContent=m;
      toast(m,'bad');busy('btnota',false);$('f_fw').disabled=false}
    x.onload=function(){
      var j={};try{j=JSON.parse(x.responseText)}catch(e){}
      if(x.status===200&&j.ok!==false){
        wrap.className='bar done';bar.style.width='100%';
        msg.textContent='升级成功，设备正在重启。请稍后刷新本页。';
        toast('升级成功，设备正在重启','ok');
        // 设备重启期间轮询 /api/status，恢复后自动刷新页面。
        // 不恢复 tickTimer —— 页面马上就要 reload 了。
        setTimeout(function(){var t=setInterval(function(){
          api('/api/status').then(function(){clearInterval(t);location.reload()})
          .catch(function(){})},3000)},8000);
      }else fail(j.err||('升级失败（HTTP '+x.status+'）'));
    };
    x.onerror=function(){fail('上传中断，请检查网络连接后重试')};
    x.ontimeout=function(){fail('上传超时，请检查网络连接后重试')};
    x.send(f);
  });
  on('btnreboot',function(){if(!confirm('确认重启设备？'))return;
    post('/api/reboot').catch(function(){});toast('设备正在重启')});
  on('btnforget',function(){if(!confirm('将清除已保存的 WiFi 配置并重启设备，是否继续？'))return;
    post('/api/forget').catch(function(){});toast('配置已清除，设备正在重启')});
  api('/api/limits').then(function(l){LIM=l;render()}).catch(function(){render()});
  api('/api/calendar').then(function(c){marks=c.marks||[];events=c.events||[];labels=c.labels||[];
    $('lab_on').checked=labels.length>0;$('labbox').hidden=!labels.length;render()}).catch(function(){render()});
  api('/api/config').then(function(c){
    $('f_ssid').value=c.ssid||'';$('f_city').value=c.city||'';
    $('f_pass').placeholder=c.has_pass?'••••••••':'尚未设置';
    $('f_key').placeholder=c.has_key?'••••••••':'尚未设置';
    $('f_host').placeholder=c.has_host?'••••••••':'xxx.re.qweatherapi.com';
    $('f_uapi').placeholder=c.has_uapi?'••••••••':'uapi-…';
  }).catch(function(){});
  tick();tickTimer=setInterval(tick,5000);
  loadPubip();
  setTimeout(function(){var obs=new IntersectionObserver(function(es){es.forEach(function(e){
    if(e.isIntersecting&&!rawRows){obs.disconnect();loadData();}
  })});obs.observe($('cv'));},200);
  window.onbeforeunload=function(){if(dirty)return '日历有未保存的修改'};
});
