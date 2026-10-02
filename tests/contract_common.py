"""Test-only JSON Schema validators; no deployed-service dependency."""
import json
import sys
from pathlib import Path
from jsonschema import Draft202012Validator

for stream in (sys.stdout,sys.stderr):
    if hasattr(stream,'reconfigure'):stream.reconfigure(encoding='utf-8',errors='backslashreplace')

ROOT=Path(__file__).resolve().parents[1]
def schema(name):
    return json.loads((ROOT/'schemas'/name).read_text(encoding='utf-8'))

HTTP=Draft202012Validator(schema('http-response-v1.schema.json'))
OCR=Draft202012Validator({'$ref':'#/$defs/OcrResult','$defs':HTTP.schema['$defs']})
CONFIG=Draft202012Validator(schema('http-service-config-v1.schema.json'))
ACCESS=Draft202012Validator(schema('access-log-v1.schema.json'))

def validate_response(value):
    HTTP.validate(value)

def validate_access(value):
    ACCESS.validate(value)

def validate_ocr_result(value):
    OCR.validate(value)
