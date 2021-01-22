//address of running fan
const ADDR = 18;
const TEST = false;
//const TEST = true;

function initWebsocket(page){
  $('#message').show();
  $('#message').html('Connecting...');
  $('#controls').hide();
  
  if (TEST) {
    test();
    return;
  }

  var url;
  var host = location.hostname;
  if (location.protocol === 'https:') {
    url = 'wss://' + host + location.pathname.substring(0,location.pathname.indexOf('/',1)) + '_ws_/' + page;
  }
  else {
    if (host == 'localhost')
      host = '192.168.1.' + ADDR;

    var port = location.port;
    if (port === '')
      port = 81;
    else
      port = parseInt(port) + 1;

    url = 'ws://' + host + ':' + port + '/' + page;
  }
  
  console.log('ws url',url);
  
  socket = new WebSocket(url);
  socket.onopen = function(){
    console.log('onopen');
    $('#message').hide();
    $('#controls').show();
  }
  socket.onmessage = function(msg){
    message(msg);
  }
  socket.onclose = function(){
    console.log('onclose');
    $('#message').show();
    $('#message').html('Disconnected');
    $('#controls').hide();
  }
}


function test() {
  // simulate connection
  $('#message').hide();
  $('#controls').show();
  
  socket = {};
  socket.send = function(data) {
    console.log('send',data);
  }

  // simulate data coming on
  setTimeout(function(){
    var obj = {};
    
    // index page
//    obj.data = '{"command":"time", "value":"Sun 8:39am"}';
//    message(obj);
//    obj.data = '{"command":"status", "value":"Off"}';
//    message(obj);
//    obj.data = '{"command":"mode", "value":"0"}';
//    message(obj);
    
    // program page
    // 4 programs, 3 times
    obj.data = '{"command":"program","value":[ [0,0,[0,0,0,0],[0,0,0,0]], [0,0,[0,0,0,0],[0,0,0,0]], [0,0,[0,0,0,0],[0,0,0,0]] ]}';
    message(obj);

    // setup page
//    obj.data = '{"date":"5/29/16", "time":"11:01am"}';
//    message(obj);
  },1000);
}
