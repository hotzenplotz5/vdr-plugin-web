#!/usr/bin/env python3

from pathlib import Path

root = Path(__file__).resolve().parents[1]
web = (root / "web.cpp").read_text(encoding="utf-8")
header = (root / "service" / "vdrsuite_hbbtv_runtime_service.h").read_text(
    encoding="utf-8"
)
presentation_header = (
    root / "service" / "vdrsuite_hbbtv_presentation_service.h"
).read_text(encoding="utf-8")
osd = (root / "webosdpage.cpp").read_text(encoding="utf-8")

required_web = (
    '#include "service/vdrsuite_hbbtv_runtime_service.h"',
    '#include "service/vdrsuite_hbbtv_presentation_service.h"',
    'strcmp(Id, VDRWEB_SERVICE_HBBTV_RUNTIME_V1) == 0',
    'vdrSuiteHbbtvRuntimeService->Handle(',
    'VdrSuiteHbbtvUiCommandType::Launch',
    'VdrSuiteHbbtvUiCommandType::Close',
    'browserClient->RedButton(runtimeCommand.channelId)',
    'browserClient->LoadUrl(VDRSUITE_HBBTV_BLANK_PAGE)',
    'browserClient->ProcessKey(key)',
    'vdrSuiteHbbtvRuntimeService->CompleteLaunch(',
    'vdrSuiteHbbtvRuntimeService->CompleteClose(',
    'VdrSuiteHbbtvPresentationStore::BeginSession(',
    'VdrSuiteHbbtvPresentationStore::EndSession(',
    'strcmp(Id, VDRWEB_SERVICE_HBBTV_PRESENTATION_V1) == 0',
    '"http://localhost/internal/blank/page"',
)

required_header = (
    '#define VDRWEB_SERVICE_HBBTV_RUNTIME_V1 "VdrWeb::HbbtvRuntime-v1"',
    'VDRWEB_HBBTV_RUNTIME_RESULT_DISCOVERY_STALE',
    'VDRWEB_HBBTV_RUNTIME_RESULT_APPLICATION_NOT_LAUNCHABLE',
    'VDRWEB_HBBTV_RUNTIME_RESULT_ACTION_UNSUPPORTED',
    'case VDRWEB_HBBTV_INPUT_OK: return "VK_ENTER";',
    'case VDRWEB_HBBTV_INPUT_RED: return "VK_RED";',
)

for token in required_web:
    if token not in web:
        raise SystemExit(f'web.cpp: missing token: {token}')

for token in required_header:
    if token not in header:
        raise SystemExit(f'runtime header: missing token: {token}')

for token in (
    '#define VDRWEB_SERVICE_HBBTV_PRESENTATION_V1 "VdrWeb::HbbtvPresentation-v1"',
    'VDRWEB_HBBTV_PRESENTATION_CHUNK_MAX 49152U',
):
    if token not in presentation_header:
        raise SystemExit(f'presentation header: missing token: {token}')

for token in (
    'VdrSuiteHbbtvPresentationStore::ApplyBgraPatch(',
    'drawImageQOI',
):
    if token not in osd:
        raise SystemExit(f'presentation capture missing token: {token}')

service_start = web.find('bool cPluginWeb::Service(')
service_end = web.find('const char **cPluginWeb::SVDRPHelpPages()', service_start)
if service_start < 0 or service_end < 0:
    raise SystemExit('unable to locate plugin Service/SVDRP boundary')

service_body = web[service_start:service_end]
runtime_block_start = service_body.find(
    'strcmp(Id, VDRWEB_SERVICE_HBBTV_RUNTIME_V1) == 0'
)
if runtime_block_start < 0:
    raise SystemExit('runtime service block missing')

runtime_block = service_body[runtime_block_start:]
for forbidden in ('param_url', 'WebApp-Url-v1.0', 'StartApplication('):
    prefix = runtime_block.split('param_url = "";', 1)[0]
    if forbidden in prefix:
        raise SystemExit(
            f'private runtime service unexpectedly exposes legacy token: {forbidden}'
        )

print("RESULT=VDRSUITE_HBBTV_RUNTIME_PROVIDER_SURFACE_PASS")
