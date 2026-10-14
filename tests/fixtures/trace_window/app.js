var statusNode = document.querySelectorAll("#status")[0];
var done = {timeout: false, ticks: 0, xhr: false, frames: 0, reported: false};
function finish() {
    if (done.reported || !done.timeout || done.ticks < 2 || !done.xhr || done.frames < 3)
        return;
    done.reported = true;
    var marker = new XMLHttpRequest();
    marker.open("GET", "/loaded.txt", true);
    marker.send();
}
setTimeout(function () { done.timeout = true; finish(); }, 10);
var interval = setInterval(function () {
    done.ticks++;
    if (done.ticks == 2) clearInterval(interval);
    finish();
}, 20);
var data = new XMLHttpRequest();
data.open("GET", "/data.txt", true);
data.onload = function () {
    statusNode.innerHTML = "async " + data.responseText;
    done.xhr = true;
    finish();
};
data.send();
function frame() {
    done.frames++;
    if (done.frames < 3) requestAnimationFrame(frame);
    finish();
}
requestAnimationFrame(frame);
document.querySelectorAll("#sync")[0].addEventListener("click", function () {
    var request = new XMLHttpRequest();
    request.open("GET", "/sync.txt", false);
    request.send();
    statusNode.innerHTML = "sync " + request.responseText;
});
