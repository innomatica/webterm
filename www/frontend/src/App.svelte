<script>
  import { onDestroy, onMount } from "svelte";
  import { JsShell } from "./lib/shell/jsShell";

  const urlPowerControl = "/api/v1/pwrctrl";
  const urlPowerState = "/api/v1/pwrstate";
  const powerBtnColorOn = "red";
  const powerBtnColorOff = "maroon";
  const powerBtnTextOn = "target powered on";
  const powerBtnTextOff = "target powered off";
  const linkBtnColorOn = "lawngreen";
  const linkBtnColorOff = "darkgreen";
  const linkBtnTextOn = "terminal enabled";
  const linkBtnTextOff = "terminal disabled";
  const iconSize = 24;
  const CSI = [
    {
      // past bracket
      regex: /\u001b\[\?2004./,
      fctn: (code) => (paste = code === "\u001b[?2004h"),
    },
  ];
  // Note: during test you can connect to localhost:5173 instead of the
  // server page hosted by the device by manually setting hostUrl as below
  // However any GET request will fail with CORS error
  // const hostUrl = "webterm.local";
  const hostUrl = window.location.host;

  let webSocket;
  let terminal;
  let escCode = "";
  let paste = false;
  let powerState = false;
  let powerBtnColor = powerBtnColorOff;
  let linkBtnColor = linkBtnColorOff;
  let powerBtnText = powerBtnTextOff;
  let linkBtnText = linkBtnTextOff;

  onMount(() => {
    checkPowerState();
    connectWebSocket();
    // create terminal
    terminal = new JsShell("#terminal", {
      // use the rest of the screen height
      // 48 (padding) + 65.051 (title-bar with bottom margin)
      height: "calc(100vh - 114px)",
      textColor: "mintcream",
      backgroundColor: "#121212",
    });
    // initially disabled
    // enableTerminal(false);
  });

  onDestroy(() => webSocket?.close());

  function enableTerminal(flag = true) {
    terminal.showCursor(flag);
    linkBtnColor = flag ? linkBtnColorOn : linkBtnColorOff;
    linkBtnText = flag ? linkBtnTextOn : linkBtnTextOff;
  }

  async function checkPowerState() {
    try {
      const url = "http://" + hostUrl + urlPowerState;
      // get data
      const resp = await fetch(url, {
        method: "GET",
        headers: { Accept: "application/json" },
      });
      if (resp.status === 200) {
        try {
          const payload = await resp.json();
          // console.log("checkPowerState.payload:", payload);
          if (payload.power) {
            if (payload.power === "on") {
              powerState = true;
            } else {
              powerState = false;
            }
          }
        } catch (e) {
          // console.log("checkPowerState.error:", e);
        }
      }
    } catch (e) {
      alert("Failed to get the power state.  Check the connection");
    }
    powerBtnColor = powerState ? powerBtnColorOn : powerBtnColorOff;
    powerBtnText = powerState ? powerBtnTextOn : powerBtnTextOff;
  }

  async function connectWebSocket() {
    if (webSocket === undefined || webSocket?.readyState === 3) {
      // creating a new websocket: not throwing exception
      // https://stackoverflow.com/questions/31002592/javascript-doesnt-catch-error-in-websocket-instantiation
      webSocket = new WebSocket("ws://" + hostUrl + "/ws");
      // register event handlers
      if (webSocket) {
        webSocket.onopen = (event) => {
          enableTerminal(true);
          // console.log("ws opened", event);
        };
        webSocket.onclose = (event) => {
          enableTerminal(false);
          // console.log("ws closed", event);
        };
        webSocket.onerror = (event) => {
          enableTerminal(false);
          // console.log("ws error:", event);
        };
        webSocket.onmessage = (event) => {
          handleIncoming(event);
        };
      }
    } else if (webSocket.readyState === 1) {
      return;
    } else {
      alert(
        webSocket.readyState === 0
          ? "socket is connecting..."
          : "socket is closing..."
      );
    }
  }

  async function onPowerBtnClick() {
    if (powerState) {
      // check power state
      checkPowerState();
    } else {
      // turn on power
      const url = "http://" + hostUrl + urlPowerControl;
      // toggle power state
      powerState = !powerState;
      // console.log("powerState:", powerState);
      // post data
      const resp = await fetch(url, {
        method: "POST",
        body: JSON.stringify({ power: powerState ? "off" : "on" }),
      });

      if (resp.status === 200) {
        // check power state
        checkPowerState();
      } else {
        // revert state since it failed
        powerState = !powerState;
      }
    }
  }

  async function onLinkBtnClick() {
    if (linkBtnColor === linkBtnColorOn) {
      webSocket?.close();
    } else {
      connectWebSocket();
    }
  }

  async function handleIncoming(event) {
    if (terminal && event.data.length) {
      let buffer = "";
      // console.log(JSON.stringify(event.data));

      for (let idx = 0; idx < event.data.length; idx++) {
        if (escCode.length) {
          escCode = escCode + event.data[idx];
          // console.log("escCode:", escCode);
        } else if (event.data[idx] === "\u001b") {
          escCode = escCode + event.data[idx];
        } else {
          buffer = buffer + event.data[idx];
          // console.log("buffer:", buffer);
        }

        for (let idy = 0; idy < CSI.length; idy++) {
          let match = escCode.match(CSI[idy].regex);
          if (match) {
            CSI[idy].fctn(escCode);
            // console.log("match:", match, paste);
            escCode = "";
            break;
          }
        }
      }
      if (buffer.length) {
        // get rid of \r
        terminal.write(buffer.replaceAll("\r", ""));
        // target device must be on if nontrivial data is received
        if (!powerState && buffer.length > 3) {
          powerState = true;
          powerBtnText = powerBtnTextOn;
          powerBtnColor = powerBtnColorOn;
        }
      }
    }
  }

  async function handleKeyDown(event) {
    // console.log("key event:", event);
    if (webSocket && webSocket.readyState === 1) {
      // Like black magic
      // https://stackoverflow.com/questions/12467240/determine-if-javascript-e-keycode-is-a-printable-non-control-character
      if (event.key.length === 1) {
        // printable chars
        if (event.ctrlKey) {
          // with Ctrl key pressed
          // C0 codes: https://en.wikipedia.org/wiki/C0_and_C1_control_codes
          if (event.keyCode >= 0x40 && event.keyCode <= 0x5f) {
            webSocket.send(new Uint8Array([event.keyCode - 0x40]));
            // console.log("sending ctrl code:", event.keyCode - 0x40);
          }
        } else {
          // plain printable chars
          webSocket.send(event.key);
        }
      } else {
        // white charaters have to be converted
        if (event.key === "Enter") {
          webSocket.send("\n");
        } else if (event.key === "Tab") {
          webSocket.send("\t");
        } else if (event.key === "Backspace") {
          webSocket.send("\b");
        } else if (event.key === "Shift" || event.key === "Control") {
          // we can ignore these here
        } else {
          // We ignore the rest of keys such as:
          //  - Escape
          //  - ArrowLeft, ArrowRight, ...
          //  - Home, End, Inset, Delete,...
          //
          // To support it check this
          // https://stackoverflow.com/questions/2876275/what-are-the-ascii-values-of-up-down-left-right
          alert(event.key + " key is not supported");
        }
      }
    }
    // console.log("paste:", paste);
  }
</script>

<main>
  <div class="header">
    <button class="tooltip" on:click={onPowerBtnClick}>
      <svg
        xmlns="http://www.w3.org/2000/svg"
        viewBox="0 0 512 512"
        width={iconSize}
        height={iconSize}
        ><!--!Font Awesome Free 6.5.2 by @fontawesome - https://fontawesome.com License - https://fontawesome.com/license/free Copyright 2024 Fonticons, Inc.--><path
          fill={powerBtnColor}
          d="M288 32c0-17.7-14.3-32-32-32s-32 14.3-32 32V256c0 17.7 14.3 32 32 32s32-14.3 32-32V32zM143.5 120.6c13.6-11.3 15.4-31.5 4.1-45.1s-31.5-15.4-45.1-4.1C49.7 115.4 16 181.8 16 256c0 132.5 107.5 240 240 240s240-107.5 240-240c0-74.2-33.8-140.6-86.6-184.6c-13.6-11.3-33.8-9.4-45.1 4.1s-9.4 33.8 4.1 45.1c38.9 32.3 63.5 81 63.5 135.4c0 97.2-78.8 176-176 176s-176-78.8-176-176c0-54.4 24.7-103.1 63.5-135.4z"
        /></svg
      >
      <span class="tooltip-right">{powerBtnText}</span>
    </button>
    <div class="title">
      <h1>ESP32 Web Terminal</h1>
      <p>{window.location.host}</p>
    </div>
    <button class="tooltip" on:click={onLinkBtnClick}>
      <svg
        xmlns="http://www.w3.org/2000/svg"
        viewBox="0 0 640 512"
        width={iconSize}
        height={iconSize}
        ><!--!Font Awesome Free 6.5.2 by @fontawesome - https://fontawesome.com License - https://fontawesome.com/license/free Copyright 2024 Fonticons, Inc.--><path
          fill={linkBtnColor}
          d="M579.8 267.7c56.5-56.5 56.5-148 0-204.5c-50-50-128.8-56.5-186.3-15.4l-1.6 1.1c-14.4 10.3-17.7 30.3-7.4 44.6s30.3 17.7 44.6 7.4l1.6-1.1c32.1-22.9 76-19.3 103.8 8.6c31.5 31.5 31.5 82.5 0 114L422.3 334.8c-31.5 31.5-82.5 31.5-114 0c-27.9-27.9-31.5-71.8-8.6-103.8l1.1-1.6c10.3-14.4 6.9-34.4-7.4-44.6s-34.4-6.9-44.6 7.4l-1.1 1.6C206.5 251.2 213 330 263 380c56.5 56.5 148 56.5 204.5 0L579.8 267.7zM60.2 244.3c-56.5 56.5-56.5 148 0 204.5c50 50 128.8 56.5 186.3 15.4l1.6-1.1c14.4-10.3 17.7-30.3 7.4-44.6s-30.3-17.7-44.6-7.4l-1.6 1.1c-32.1 22.9-76 19.3-103.8-8.6C74 372 74 321 105.5 289.5L217.7 177.2c31.5-31.5 82.5-31.5 114 0c27.9 27.9 31.5 71.8 8.6 103.9l-1.1 1.6c-10.3 14.4-6.9 34.4 7.4 44.6s34.4 6.9 44.6-7.4l1.1-1.6C433.5 260.8 427 182 377 132c-56.5-56.5-148-56.5-204.5 0L60.2 244.3z"
        /></svg
      >
      <span class="tooltip-left">{linkBtnText}</span>
    </button>
  </div>
  <!-- svelte-ignore a11y-no-static-element-interactions -->
  <div id="terminal" on:keydown={handleKeyDown}></div>
</main>

<style>
  main {
    /* display: flex;
    flex-direction: column; */
    padding: 1.5rem;
    /* width: 100%; */
  }
  .header {
    display: flex;
    align-items: end;
    justify-content: space-between;
    margin-bottom: 8px;
  }
  .title {
    text-align: center;
  }
  .title h1 {
    margin: 0;
    color: aliceblue;
  }
  .title p {
    margin: 0;
    color: silver;
    font-weight: 300;
  }
  button {
    background-color: transparent;
    border-color: transparent;
  }

  .tooltip {
    position: relative;
    display: inline-block;
  }

  .tooltip .tooltip-right {
    visibility: hidden;
    width: 150px;
    background-color: transparent;
    font-size: 14px;
    color: silver;
    text-align: center;
    border-radius: 6px;
    padding: 5px 0;

    /* Position the tooltip */
    position: absolute;
    /* left: 24px; */
    z-index: 1;
  }

  .tooltip .tooltip-left {
    visibility: hidden;
    width: 150px;
    background-color: transparent;
    font-size: 14px;
    color: silver;
    text-align: center;
    border-radius: 6px;
    padding: 5px 0;

    /* Position the tooltip */
    position: absolute;
    right: 24px;
    z-index: 1;
  }

  .tooltip:hover :is(.tooltip-left, .tooltip-right) {
    visibility: visible;
  }
</style>
