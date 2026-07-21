[English](README.md) | [한국어](README.ko.md)

# Claude Desktop Display

ESP32-C3에 연결한 캐릭터 LCD에 내 Claude **5시간** / **주간** rate-limit
사용량을 띄워주는 독립형 데스크 기기. PC를 계속 켜둘 필요 없음 - 기기가
Anthropic API에 직접 붙는다.

```
 ESP32-C3  <--HTTPS, OAuth Bearer token-->  api.anthropic.com
(SDA=GPIO4, SCL=GPIO5)                      (real Messages endpoint)
```

[claude-usage-stick](https://github.com/oauramos/claude-usage-stick) 프로젝트
(그리고 Claude Code CLI 자기 자신)가 한도를 확인하는 방식과 똑같다: Claude
Code OAuth 토큰으로 `/v1/messages`에 `max_tokens: 1`짜리 작은 요청을 보내고,
응답 헤더에서 바로 사용률을 읽는다 - `anthropic-ratelimit-unified-5h-utilization`,
`anthropic-ratelimit-unified-7d-utilization`. 진짜 API 엔드포인트라서 뚫어야 할
Cloudflare 봇 차단도 없고 빼내야 할 브라우저 쿠키도 없다. 이 프로젝트의 원래
영감이 된 macOS 앱 [ClaudeMeter](https://github.com/eddmann/ClaudeMeter)처럼
claude.ai 내부 웹 API를 스크래핑하는 것과는 다르다.

## 하드웨어

- ESP32-C3 보드 (DevKitM-1, "SuperMini", Xiao ESP32C3 등 아무 변형이나)
- **PCF8574 I2C 백팩**이 달린 캐릭터 LCD - 이 빌드는 16x2로 설정되어 있다
  (`firmware/src/main.cpp`의 `LCD_COLS`/`LCD_ROWS`); 18x2, 20x4도 이
  상수 두 개만 자기 모듈에 맞게 바꾸면 된다
- 모멘터리 푸시 버튼 1~2개 (둘 다 선택 사항):
  - 화면 버튼 - 화면을 수동으로 넘길 때 씀. 없어도 자동 로테이션만으로
    잘 동작한다
  - 디버그 버튼 - 시리얼 디버그 콘솔을 연다 (아래 [디버그
    모드](#디버그-모드) 참고). 시리얼로 `d`를 입력해도 똑같이 되니 없어도
    무방하다
- 점퍼 와이어

### 배선

| LCD 백팩 핀 | ESP32-C3 핀 |
|---|---|
| GND | GND |
| VCC | 5V (또는 3V3 - 아래 참고) |
| SDA | GPIO4 |
| SCL | GPIO5 |

화면 버튼(선택 사항): 한쪽 다리는 **GPIO20**, 다른 쪽은 **GND**로.
디버그 버튼(선택 사항): 한쪽 다리는 **GPIO21**, 다른 쪽은 **GND**로.
둘 다 내부 풀업을 쓰기 때문에 저항은 필요 없다. 보드에 GPIO20/21이 안
나와 있다면 `main.cpp`의 `PIN_SCREEN_BUTTON`/`PIN_DEBUG_BUTTON`을
SDA/SCL이나 스트래핑 핀(ESP32-C3의 GPIO2/8/9)이 아닌 다른 빈 핀으로
바꾸면 된다. 둘 다 원래 UART0 RX/TX인데, 이 프로젝트는 시리얼을 네이티브
USB-CDC로 쓰기 때문에 문제없다 - 다만 클래식 UART를 기대하는 다른 코드랑
같이 쓴다면 참고할 것.

> 대부분의 PCF8574 백팩과 그게 구동하는 HD44780 LCD는 밝은 백라이트/명암비를
> 위해 5V를 원한다. I2C 버스 자체는 이런 모듈 대부분에서 ESP32-C3의 3.3V
> 로직 핀으로 5V 전원 백팩을 읽어도 실제로는 문제없다 (레벨 시프터 불필요).
> 보드에 5V 핀이 없다면 (USB Vbus passthrough) 3V3도 되긴 하는데, 좀 더
> 어둡다.

화면에 아무것도 안 뜨면 LCD/백팩 뒷면의 작은 가변저항을 돌려볼 것 - 명암비
조정용이고, 기본 상태로 완전히 낮춰져 있는 경우가 많다.

## 설정

### 1. Claude Code OAuth 토큰 받기

[Claude Code CLI](https://docs.anthropic.com/en/docs/claude-code)가 설치되어
로그인되어 있는 아무 머신에서:

```bash
claude setup-token
```

이런 비대화형 용도로 쓰라고 만들어진 장기 유효 OAuth 토큰이 출력된다. 복사해
둘 것.

### 2. 펌웨어 설정 및 플래시

[PlatformIO](https://platformio.org/)(CLI 또는 VS Code 확장)가 필요하다.

```bash
cd firmware
cp src/secrets.example.h src/secrets.h
# src/secrets.h 편집: WiFi SSID/비밀번호 + 1단계에서 받은 토큰
pio run --target upload
pio device monitor
```

시리얼 모니터에 LCD의 감지된 I2C 주소, WiFi 연결 진행 상황, 매 폴링의 HTTP
상태가 표시된다. LCD가 16x2가 아니라면 `firmware/src/main.cpp` 맨 위의
`LCD_COLS`/`LCD_ROWS`만 바꾸면 된다 - 사용률 막대가 설정한 폭에 맞춰
자동으로 넓어지거나 좁아진다. 실제 몇 칸까지 보이는 LCD인지 잘 모르겠다면
아래 [디버그 모드](#디버그-모드)의 `r` 명령을 참고할 것.

처음 전원을 넣으면 WiFi 연결을 시작하기 전에 잠깐 "✳ Claude" / "Desktop
Display" 두 줄짜리 스플래시가 뜬다 (✳는 작은 커스텀 반짝임 문자이고, 실제
로고를 재현한 건 아니다).

이게 끝 - 브릿지 스크립트도, 계속 켜둬야 할 두 번째 머신도 필요 없다.

## 디스플레이 읽는 법

화면은 몇 초 간격으로 (`main.cpp`의 `SCREEN_ROTATE_MS`) 두 화면을 번갈아
보여준다 - 버튼이 연결되어 있다면 눌러서 기다리지 않고 바로 다음 화면으로
넘어갈 수 있다:

```
5H █████------ ✳
42% 3h 5m Left
```

```
WK ███████---- ✳
67% Fri 19:00
```

(막대 전체에 위아래 얇은 선이 끊김 없이 이어지고, 각 칸은 그 테두리
바로 안쪽에 1픽셀짜리 여백을 둬서 로딩바 느낌을 낸다 - `-`는 빈 칸이라는
표시고 빈 공백이 아니다 - 0%일 때도 막대 경계가 보이도록. 실제 막대
해상도는 칸 단위보다 더 세밀하다 - 아래 참고. 1번째 줄 끝의 반짝임은
신선도 표시를 겸한다 - 아래 "stale" 항목 참고 - 2번째 줄은 아무 장식 없는
텍스트다. 이 기기의 16칸짜리 LCD로는 퍼센트/카운트다운 길이 조합마다
안정적으로 자리가 남는다는 보장이 없기 때문. 둘 다 한 화면에 보여주는
세 번째 "한눈에 보기" 화면도 코드에는 있지만 지금은 꺼져 있다 -
`main.cpp`의 `SCREEN_COUNT`.)

- **5H** - 5시간 롤링 세션 한도의 사용률과, 리셋까지 남은 시간 ("3h 05m"이
  아니라 "3h 5m" - 분은 0으로 안 채운다. 1시간 미만 남으면 시간 부분이
  아예 없어진다 - "0h 45m Left"가 아니라 "45m Left").
- **WK** - 7일(주간) 롤링 한도의 사용률과, 리셋되는 요일 + 시각 (여러 날에
  걸친 기간은 시간 단위 카운트다운보다 이쪽이 더 읽기 편하다). `main.cpp`의
  `DISPLAY_TZ_OFFSET_SEC` 시간대로 표시된다 - 이 빌드는 KST(UTC+9)로
  하드코딩되어 있으니, 다른 시간대에서 쓴다면 이 상수를 바꿀 것.
- 막대 자체도 한 칸 단위보다 더 세밀한 해상도를 쓴다 (`main.cpp`의
  `BAR_SUBDIV`, 칸당 5단계) - 커스텀 문자 몇 개를 추가로 써서 꽉 찬
  칸/빈 칸만 있는 게 아니라 부분적으로 채워진 칸도 표현한다.
- 기기가 한동안 제대로 된 값을 못 받아왔다면 (WiFi 끊김, API 타임아웃 등)
  1번째 줄의 반짝임이 빈칸이 되고 2번째 줄의 상세 정보가 "OLD"로 바뀐다 -
  퍼센트 자체는 그대로 둔다 (여전히 마지막으로 알던 값이라) 화면이 아예
  비어 보이지 않도록.
- 시간 정보가 `--`로 나온다면 아직 유효한 리셋 시각을 못 받았거나, (5H
  화면만) 부팅/재연결 직후라 NTP로 시계를 아직 못 맞춘 것이다 (문제 해결
  참고).
- Anthropic이 401을 반환하면 두 화면 대신 **"Auth failed! / Redo
  setup-token"**이 뜬다 - 토큰이 만료됐거나 폐기된 것. `claude setup-token`을
  다시 실행하고 `secrets.h`를 갱신할 것.

폴링은 기본적으로 2분마다 일어난다 (`main.cpp`의 `REFRESH_INTERVAL_MS`) -
매번 최소한이라도 실제 API 호출이라서, 몇 초마다 두드릴 필요는 없다.

## 디버그 모드

디버그 버튼(GPIO21, 연결되어 있다면)을 누르거나 - 보드가 USB로 연결만
되어 있다면 버튼 없이 시리얼 모니터에 `d`만 입력해도 - 자동 스윕 대신
시리얼 포트로 인터랙티브 콘솔이 열린다. "100%에 59분 남았을 때" 같은
특정 값 하나가 화면에서 어떻게 나오는지, 실제 사용량 데이터나 달력/시계가
자연스럽게 그 값을 거쳐갈 때까지 기다리지 않고 바로 확인할 수 있다.

시리얼 모니터를 연 상태에서 (`pio device monitor`) 한 줄에 명령 하나씩
입력하고 엔터를 치면 된다:

| 명령 | 동작 |
|---|---|
| `p<0-100>` | 퍼센트 설정 (현재 보고 있는 화면에 적용) |
| `t<h>:<m>` | 5H 화면: 카운트다운을 h시간 m분 남음으로 설정 |
| `w<0-6>` | WK 화면: 요일 설정 (`0`=월 .. `6`=일) |
| `k<h>:<m>` | WK 화면: 리셋 시각 설정 |
| `+` / `-` | 마지막으로 설정한 값(퍼센트/카운트다운/요일/시각)을 한 단위씩 증감 |
| `s` | stale 표시 토글 (1번째 줄 반짝임 빈칸 + 2번째 줄 "OLD") |
| `r<text>` | 레이아웃 로직 없이 2번째 줄에 `<text>`를 그대로 출력 |
| `q` | 일반 동작으로 복귀 |

매 명령마다 실제 5H/WK 화면이 쓰는 것과 완전히 동일한 코드로 즉시 다시
그리고, 2번째 줄의 결과 텍스트(및 길이)를 시리얼로도 그대로 에코해준다 -
작은 LCD 글자를 눈으로 힘들게 읽는 대신 확인하기 편하다.

`r` 명령은 자기 LCD가 실제로 몇 칸까지 보여주는지 확인하는 가장 쉬운
방법이기도 하다 - `r0123456789ABCDEFGH`를 보내고 뒤쪽 어느 문자부터
화면에 안 뜨는지 보면 된다 (아래 [문제 해결](#문제-해결) 참고).

`q`를 누르면 일반 동작으로 복귀한다 - 디스플레이에만 영향을 줄 뿐,
아무것도 저장하거나 영구적으로 바꾸지 않는다.

## 레이아웃 도구

[`lcd_editor.html`](lcd_editor.html)은 위 화면들 같은 레이아웃을 목업할 때
쓰는 독립형 오프라인 레이아웃 그리드다 - 브라우저에서 열어 칸에 바로
타이핑하면 코드로 옮기기 전에 레이아웃이 어떻게 보일지 확인할 수 있다.
순수하게 시각적인 도구고(코드 생성 없음) 빌드 과정도, 의존성도 없다.

## 문제 해결

- **LCD가 비거나 깨져 보임** - 백팩의 명암비 가변저항을 조정할 것. 시리얼
  모니터 로그에서 감지된 I2C 주소도 확인해볼 것 (일부 백팩은 흔한 `0x27`
  대신 `0x3F`를 쓴다 - 펌웨어가 버스를 자동 스캔하니 이건 그냥 본인 확인용).
- **"Connecting WiFi"에서 멈춤** - ESP32-C3는 2.4GHz WiFi만 지원한다. 5GHz
  전용 SSID를 가리키고 있는 건 아닌지 확인할 것.
- **매번 "DISCONN r=2"(AUTH_EXPIRE)에서 멈춤, 비밀번호는 맞고 스캔에도
  네트워크가 보임** - 일부 ESP32-C3 SuperMini/클론 보드는 기본 TX 출력이
  반사되어 인증 핸드셰이크 자체를 깨뜨리는 실제 안테나 임피던스 매칭 결함이
  있다. 어느 AP에 붙든 동일하게 실패한다
  ([arduino-esp32 #6767](https://github.com/espressif/arduino-esp32/issues/6767)).
  펌웨어가 이미 이걸 우회하고 있어서 (`connectWiFi()`의 `WiFi.begin()`
  직후에 `WiFi.setTxPower(WIFI_POWER_8_5dBm)` 호출) 따로 할 일은 없다 - 다만
  WiFi 코드를 수정하다가 이 증상이 다시 나타난다면 이유는 이것이다.
- **화면에 "Auth failed!" / 시리얼 로그에 HTTP 401** - OAuth 토큰이
  만료됐거나 폐기된 것. `claude setup-token`을 다시 실행하고 `secrets.h`를
  갱신한 뒤 재플래시할 것.
- **401 말고 다른 HTTP 에러로 계속 "OLD" 상태** - ESP32가 (LAN뿐 아니라)
  실제 인터넷 접속이 되는지 확인할 것.
- **글자가 오른쪽에서 잘려 보이거나, 1번째 줄 반짝임이 아예 안 보임** -
  LCD가 `LCD_COLS`로 설정된 것보다 실제로는 더 적은 칸만 보여주는 걸 수도
  있다 (이 프로젝트가 쓰는 기기도 18칸으로 알고 있었는데 실제로는 16칸만
  보였다). 디버그 콘솔의 `r` 명령으로 확인할 것: 디버그 버튼을 누른 다음
  `r0123456789ABCDEFGH`를 보내고, 실제 화면에서 뒤쪽 어느 문자부터 안
  보이는지 확인 - 그게 실제 사용 가능한 폭이다. `LCD_COLS`를 거기에 맞춰
  바꿀 것.
- **5H 화면 카운트다운이 시각 대신 `--`로 나옴** - 기기 자신의 시계가
  필요해서 NTP로 맞춘다 (`setup()`의 `configTime()`). WiFi가 붙은 직후
  몇 초간은 `--`가 나오는 게 정상이다. 계속 안 없어진다면 ESP32가 진짜
  인터넷 접속이 되는지 확인할 것 (NTP는 HTTP(S)가 아니라 UDP/123
  아웃바운드가 필요하다). (WK 화면의 리셋 요일/시각은 이게 필요 없다 -
  기기 시계와 무관하게 API가 준 리셋 타임스탬프에서 바로 계산한다.)
- **퍼센트는 나오는데 두 화면 다 시간 정보만 계속 `--`** -
  `anthropic-ratelimit-unified-{5h,7d}-reset` 헤더가 안 왔거나 예상과
  다른 형식으로 온 것. `fetchUsage()`의 시리얼 로그 줄
  `[API] 5h=... reset5h='...' 7d=... reset7d='...'`을 확인할 것 - 2026-07-21
  기준으로는 그냥 유닉스 타임스탬프 숫자다 (예: `1784652600`),
  `main.cpp`의 `parseResetEpoch()`가 이걸 파싱한다.

## 보안 참고사항

버튼이 달린 보드용으로 만들어져서 토큰을 PIN으로 암호화해 저장하는
claude-usage-stick과 달리, 이 프로젝트는 OAuth 토큰을 `secrets.h`/플래시에
평문으로 저장한다 - WiFi 비밀번호를 저장하는 방식과 똑같다. 본인 책상 위에
놓인 개인용 기기라면 합리적인 트레이드오프지만, 보드나 컴파일된 펌웨어에
물리적/USB로 접근할 수 있는 사람이라면 누구든 꺼낼 수 있다. 이게 신경 쓰인다면
claude-usage-stick의 `crypto.cpp`(AES-256-GCM + PIN)를 참고할 것 - PIN 입력용
버튼만 있으면 되는데, 이 빌드는 그게 배선되어 있지 않다.

## 면책 조항

이건 비공식 커뮤니티 프로젝트로, Anthropic과 제휴하거나 승인받지 않았다.
Claude Code 클라이언트 밖에서 개인 Claude Code OAuth 토큰을 재사용해 Claude
Code 자신이 읽는 것과 같은 rate-limit 헤더를 폴링한다 - Anthropic이 언제든
이를 변경하거나 제한할 수 있다. 본인 책임 하에 사용할 것. 토큰은 이 기기에서
`api.anthropic.com`으로만 직접 전달되며, 어떤 제3자에게도 전송되지 않는다.

## 크레딧

- [eddmann/ClaudeMeter](https://github.com/eddmann/ClaudeMeter) - 원래 영감
  (macOS 메뉴바 앱; 이 프로젝트는 직접-API 방식으로 바꾸기 전엔 이 앱의
  하드웨어 동반 기기로 시작했다)
- [oauramos/claude-usage-stick](https://github.com/oauramos/claude-usage-stick) -
  여기서 쓰는 OAuth + rate-limit-header 기법과 CA 번들의 출처 (MIT)
- [caffentrager/esp32-wifi-fix-kit](https://github.com/caffentrager/esp32-wifi-fix-kit) -
  여기서 stale-NVS-cache 정리와 방어적 HT20 대역폭 강제에 쓰는
  `Esp32WifiFix` 라이브러리(`firmware/lib/Esp32WifiFix/`)의 출처. 사실 이
  프로젝트에서 진행한 WiFi 디버깅(자세한 내용은 [readai.md](readai.md))이
  그 킷의 AUTH_EXPIRE 근본 원인 설명을 위에서 언급한 안테나 결함 쪽으로
  정정하게 만든 계기였다 (MIT)

## 라이선스

MIT.
