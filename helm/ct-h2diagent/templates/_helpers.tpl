{{- define "ct-h2diagent.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{- define "ct-h2diagent.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{- define "ct-h2diagent.labels" -}}
helm.sh/chart: {{ include "ct-h2diagent.chart" . }}
{{ include "ct-h2diagent.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{- define "ct-h2diagent.selectorLabels" -}}
app.kubernetes.io/name: {{ include "ct-h2diagent.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}
