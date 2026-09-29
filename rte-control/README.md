# Multi-Target RTE HTTP control

현재 프로젝트의 4표적 HDL DUT를 제어하는 C11/Mongoose HTTP daemon과 호스트 웹 GUI다. RF는 libiio **local** backend, FPGA는 기존 MathWorks `/dev/mwipcore0`의 ioctl/mmap을 사용한다. 커널 드라이버와 HDL 자체는 변경하지 않는다.

## 운용 계약

- RF 입력: 공통 RX/TX 중심 주파수, 공통 RX/TX 대역폭, TX gain(dB), RX gain(dB). TX 음수 dB는 감쇠를 뜻한다.
- LO는 드라이버 PLL/clock 반올림 때문에 요청값과 최대 5 Hz 차이날 수 있다. RX/TX 읽기값은 서로 같아야 하며 대역폭/게인은 정확히 일치해야 한다. 이미 오차 범위 내인 LO는 다시 쓰지 않는다. GUI 입력과 저장 파일은 요청 주파수를 유지하고 상태와 FPGA 인코딩에는 실제 읽기값을 사용한다.
- 샘플레이트는 RX/TX **61.44 MS/s 고정**이다. API, GUI, 부팅 복원, 오류 복구 어느 경로에서도 `sampling_frequency`나 `gain_control_mode`를 쓰지 않는다. 기존 RX manual 설정을 읽어 검증한다.
- 표적 1~4를 독립 패널 네 개로 동시에 표시한다. 각 패널의 다섯 입력을 편집한 뒤 그 패널의 **loadParam 적용**을 눌러 해당 표적만 적용한다. 편집·RF 변경·부팅 복원은 LP를 구동하지 않는다. 적용 요청 중에는 네 표적과 RF의 적용 버튼을 잠가 한 번에 요청 하나만 처리한다.
- 표적 요청은 해당 표적의 ID/FD/FRQ/PHOF/SC/EN과 LP만 쓴다. 복구도 같은 표적에 한정된다. 다른 표적의 초기화 여부는 첫 적용의 전제 조건이 아니다. 모든 표적의 동시 clock 적용 기능은 제공하지 않는다.
- 중심 주파수가 바뀌면 기존 FPGA word를 유지하고 `needs_reapply`를 표시한다. 사용자가 표적별로 재적용한다. 대역폭/게인 변경은 이 이유로 재적용을 요구하지 않는다.
- 최초 부팅 및 데몬 재시작 시 active bank를 추정하지 않는다. 저장 표적은 초안으로 복원되고 각각 직접 적용해야 한다. 선택 표적 이외 LP가 이미 high이면 쓰기를 거부한다.

FPGA timestamp는 **2609182026**, 물리 주소는 `0x43c00000`, mapping 크기는 `0x10000`이다. 현재 IP와 다른 timestamp는 거부한다. 특히 SC3=`0x158`, SC4=`0x148`이며 주소를 등차수열로 계산하지 않는다. 전체 주소표는 [ABI](include/rte/abi.h)와 [descriptor](src/model.c)에 있다.

## 보드 준비와 접속

현재 프로젝트의 XSA로 생성한 펌웨어를 로드한 뒤, 사용자가 보드에서 다음을 수행한다.

```sh
fw_setenv attr_name compatible
fw_setenv attr_val ad9364
reboot
```

`ad9361-phy`의 device-tree compatible이 `ad9364` 또는 `adi,ad9364`여야 한다. RX manual과 고정 Fs도 확인한다. 설정 전에는 daemon이 진단 메시지를 남기고 시작에 실패한다. U-Boot 환경을 daemon이 대신 수정하지 않는다.

기본 GUI 주소는 `http://192.168.2.1:8080/`이다. USB IP는 U-Boot `ipaddr`를 따른다. `/etc/default/rte-httpd`에서 listen URL, 장치, 정적 자산 경로와 영속 저장을 설정할 수 있다. `S60rte-httpd`가 장치 준비와 `/api/v1/system`의 HTTP 응답을 확인한다. 하드웨어가 degraded여도 진단 GUI는 유지하며, 하드웨어 상태는 `/healthz`로 확인한다. USB Ethernet/serial/mass storage는 유지하며 원격 IIO/iiod 경로는 사용하지 않는다.

첫 화면에 네 표적의 입력·마지막 적용 상태·편집 상태·개별 버튼을 함께 표시한다. 너비 1200 CSS px 이상은 4열, 681~1199 px은 2×2, 680 px 이하는 1열로 배치한다. 패널마다 미적용 편집값과 초기화를 독립 관리하며, 한 패널을 적용해도 다른 패널의 편집값은 유지된다. RF 설정과 상태/진단은 표적 패널 아래에 배치한다.

공통 대역폭의 40 MHz 상한은 현재 Linux 드라이버의 `_available` 범위(RX 0.2~56 MHz, TX 0.2~40 MHz)의 교집합에서 정했다. `ad9361_validate_rf_bw()`는 AD9364에서 56 MHz까지 허용하지만 TX `_available`은 40 MHz로 선언되어 있으므로, 현재 서버는 드라이버가 공시하는 범위에 맞춘다. 40 MHz를 칩 자체의 절대 한계로 해석하지 않는다.

기본 저장 파일은 `/mnt/jffs2/rte-multitarget/last-config.json`이다. schema version 2, profile `multitarget-v2`, timestamp, RF 네 값, 표적별 마지막 성공 설정(null 허용)을 저장한다. 2초 동안 저장을 병합하고 임시 파일·fsync·rename으로 교체한다. 파일의 스키마와 거리·디지털 계수 범위를 검증한 후 RF만 복원한다. RF 변경으로 저장 초안의 Doppler가 새 범위를 벗어난 경우에도 초안은 보존하며, 사용자가 loadParam을 누를 때 현재 LO 기준으로 전체 인코딩을 다시 검증한다. 이전 단일 표적 파일은 읽지 않는다. 호환되지 않는 파일/복원 실패/저장 실패는 응답의 warning에 나타난다.

## API

전체 명세: [docs/openapi.yaml](docs/openapi.yaml).

| 요청 | 의미 |
|---|---|
| `GET /healthz` | 정상/복구 실패 상태. 표적 미적용 자체는 장애가 아니다. |
| `GET /api/v1/system` | RF와 네 표적의 전체 snapshot, revision/ETag |
| `GET /api/v1/rf/config` | 같은 전체 snapshot |
| `GET /api/v1/rte/config`, `/api/v1/rte/status` | 같은 전체 snapshot |
| `GET /api/v1/rte/targets/{1..4}` | 같은 전체 snapshot |
| `GET /api/v1/rf/capabilities` | 드라이버 `_available`의 RX/TX 교집합과 gain 간격 |
| `GET /api/v1/rte/capabilities` | 현재 RF 기준 변환 범위·해상도·지연 기준 |
| `PATCH /api/v1/rf/config` | RF 네 입력 중 하나 이상. FPGA write 없음 |
| `POST /api/v1/rte/targets/{1..4}/load-param` | 해당 표적의 완전한 다섯 입력을 인코딩하고 LP 적용 |

모든 HTTP 변경은 GET 응답의 ETag를 `If-Match`에 담아야 한다. 누락은 428, revision 충돌은 409, 잘못된 입력은 422다. 일괄 표적/통합 RF+표적 API와 raw register 쓰기 API는 없다. `sample_rate_hz`, 과거 호흡·심박 필드, 알 수 없는/중복 key를 거부한다.

RF 요청 예:

```json
{"carrier_hz":2450000000,"bandwidth_hz":30000000,"tx_gain_db":-10.25,"rx_gain_db":50}
```

표적 요청 예 (`POST /api/v1/rte/targets/3/load-param`):

```json
{"enabled":true,"range_m":250,"radial_velocity_mps":10,"gain_linear":0.015625,"phase_offset_deg":0}
```

상태 GET은 **마지막 확인한 snapshot**이다. RF capabilities 조회는 실제 RF를 읽어 범위를 구하고 외부 RF 변경을 관측하면 snapshot/revision도 갱신한다. `last_hardware_check_ms`는 Unix 시간이 아닌 보드 `CLOCK_MONOTONIC` 밀리초다. 매 변경 전 실제 RF/timestamp와 이미 적용한 선택 표적의 staged bank를 확인한다. 외부 RF 변경이 발견되면 revision을 갱신하고 stale 요청을 거부한다. 현재 IP에는 active bank 전체의 독립 readback이 없다. `requested`와 `applied.registers`는 마지막 성공 기록이며, `hardware_state_known`, `degraded`, `needs_reapply`를 함께 해석해야 한다. 재부팅 시 `saved`는 초안이고 `requested/applied`는 null이다.

`applied.radial_velocity_mps`는 마지막 인코딩 당시 양자화 결과다. `encoded_carrier_hz`를 유지하고 현재 마지막 확인한 RF로 환산한 값은 `effective_radial_velocity_mps`로 제공한다. 상태 미확인/복구 실패이면 effective 값은 null이다. RF `requested_rf`는 마지막 성공한 RF 요청의 완성 값이고 `rf`는 마지막 실제 확인값이다. 일부 실패 후 검증된 복구는 기존 성공 기록을 유지하고, 복구 실패는 degraded로 표시하여 쓰기를 차단한다. timeout 뒤에는 상태를 재조회하고 요청을 자동 반복하지 않는다.

RF 복구 중 이전 LO가 다시 반올림되면 그 실제값으로 snapshot/revision과 표적 재적용 필요 상태를 갱신한다. `/api/v1/rf/capabilities`의 `carrier_hz.readback_tolerance_hz`는 5이며 발진기의 물리적 주파수 정확도 사양을 의미하지 않는다. [RF 적용 오류 검토](../../docs/pluto-rte-rf-apply-fix.ko.md)에 원인, 재현 조건과 보드 재시험 순서를 기록했다.

## 변환과 측정 기준

지연 양자화는 `Q=round(64*(2*R*Fs/c-18))`, `ID=Q/64`, `FD=Q%64`이다. 현재 생성 RTL의 **DUT 디지털 입출력 고정 지연은 18샘플**로 xsim에서 검증했다. 상세 결과와 재실행 방법은 [RTL 검증](tests/rtl/README.md)을 참고한다. 거리 범위는 약 43.915~2542.147 m, 간격은 약 0.03812 m이다. API의 정확한 capabilities를 사용한다. 이는 RF 포트 간 교정값이 아니며 `rf_calibrated=false`다.

양의 속도는 멀어짐이며 Doppler는 `-2*v*fc/c`다. PHOF에는 거리 위상과 사용자가 입력한 추가 위상을 포함한다. 디지털 계수는 `round(gain_linear*2^20)`으로 표현한다. 표현 최대값 약32는 왜곡 없는 사용 최대값을 뜻하지 않는다. 현재 DUT의 16-bit 합산과 출력 4-bit shift에서 wrap이 발생할 수 있으므로 입력 진폭과 네 표적의 합을 고려해야 한다. HDL 연산은 변경하지 않았다.

## 빌드와 검증

Buildroot의 `zynq_pluto_defconfig`는 rte-control, Mongoose 7.8, libiio local backend를 포함한다. 상위 `plutosdr-fw/Makefile`은 명시한 `XSA_FILE`을 우선하고 파일 내용 hash가 바뀌면 bitstream을 다시 추출한다. Vivado가 없고 현재 프로젝트의 XSA를 명시하지 않았다면 Pluto 빌드는 중단한다. 기본 ADI bitstream을 조용히 패키징하지 않는다.

```sh
# plutosdr-fw에서, 현재 생성 IP를 포함하는 XSA를 지정한다.
make TARGET=pluto XSA_FILE=/absolute/path/to/current-project/system_top.xsa
```

앱만 호스트에서 검증하려면 libiio 개발 헤더/라이브러리, C compiler, Node.js, Python 3, Mongoose 7.8 소스가 필요하다.

```sh
cd plutosdr-fw/rte-control
make test test-ui
make test-http httpd MONGOOSE_SOURCE=/path/to/mongoose-7.8/mongoose.c \
    MONGOOSE_CFLAGS=-I/path/to/mongoose-7.8
python3 tests/test_firmware_integration.py
```

`test`는 모델/레지스터 격리·복구/controller/실제 IIO API 호출 경계 테스트를 실행한다. HTTP 테스트는 엄격한 JSON, 단일 표적 라우팅, ETag, 정적 자산, RF 복원과 표적 초안 보존을 검증한다. GUI 테스트는 DOM/fetch 모의 환경에서 동작하며 실제 브라우저/보드 시험을 대체하지 않는다. ARM 크로스 빌드는 `CC`, `AR`, sysroot, libiio 및 Mongoose 경로를 지정한다. `BUILD_DIR`를 분리하면 호스트 산출물과 공존할 수 있다.

`rte-regtool encode 3 2450000000 250 10 0.015625 0 1`은 하드웨어 접속 없이 인코딩을 확인한다. `info`와 `readback [1..4]`는 장치를 열어 읽는다. 장치는 daemon이 독점 소유하므로 진단 도구를 실행하기 전에 서비스를 중지해야 한다. 일반 쓰기는 HTTP의 표적별 적용 경로를 사용한다.

호스트 테스트, ARM 앱 크로스 빌드와 생성 RTL 시뮬레이션을 수행했다. 사용자의 전체 `make` 결과 검토 후 서비스 재시작 동작을 보완했고, 이후 보드 API 읽기와 사용자의 RF 오류 보고를 바탕으로 PLL readback 검증을 수정했다. 수정된 앱과 GUI를 포함하여 `.frm`을 다시 패키징했다. 이미지 내부 파일·DTB·bitstream·라이브러리도 검사했다. 보드에 수정 펌웨어를 올리는 작업과 수정 후 RF 적용 시험은 사용자가 수행한다. 업로드 후 순서와 기대값은 [보드 작업 절차](../../docs/pluto-rte-board-bringup.ko.md)에 정리했다.
