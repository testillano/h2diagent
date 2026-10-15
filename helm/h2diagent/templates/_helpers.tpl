{{/*
Expand the name of the chart (peer name). nameOverride wins so the wrapper can
instantiate this chart twice ("server"/"client") without resource collisions.
*/}}
{{- define "h2diagent.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" }}
{{- end }}

{{- define "h2diagent.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
{{- end }}

{{/*
Common labels
*/}}
{{- define "h2diagent.labels" -}}
helm.sh/chart: {{ include "h2diagent.chart" . }}
{{ include "h2diagent.selectorLabels" . }}
{{- if .Chart.AppVersion }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end }}

{{/*
Selector labels
*/}}
{{- define "h2diagent.selectorLabels" -}}
app.kubernetes.io/name: {{ include "h2diagent.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end }}

{{/*
Whether this peer runs a Diameter listener (server or both, with a non-zero port).
*/}}
{{- define "h2diagent.hasServer" -}}
{{- if and (or (eq .Values.role "server") (eq .Values.role "both")) (gt (int .Values.diameter.serverPort) 0) -}}
true
{{- end -}}
{{- end }}

{{/*
Effective "brain" (companion h2agent) host the gateway should dial, or empty.
  - sidecar  (h2agent.enabled=true)                 -> localhost
  - external (enabled=false + h2agent.host set)     -> that host
  - none     (enabled=false + h2agent.host empty)   -> "" (no brain)
*/}}
{{- define "h2diagent.brainHost" -}}
{{- if .Values.h2agent.enabled -}}
localhost
{{- else -}}
{{- .Values.h2agent.host -}}
{{- end -}}
{{- end }}

{{/*
Effective "brain" port (traffic port of the companion h2agent).
*/}}
{{- define "h2diagent.brainPort" -}}
{{- if .Values.h2agent.enabled -}}
{{- .Values.h2agent.trafficPort -}}
{{- else -}}
{{- .Values.h2agent.port -}}
{{- end -}}
{{- end }}
