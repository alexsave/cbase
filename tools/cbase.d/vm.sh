# shellcheck shell=bash
# vm.sh - the VM side of a deploy: PHASE 3, not built yet (README.md "./cbase deploy tool").
#
# Phase 3 fills this file, and deploy calls it where it calls vm_deploy now:
#   - the VM group found by its tag (aws.sh aws_group_find vm) and joined, the port claimed in the
#     group's state file (state.sh state_change vm ...);
#   - the group's free memory checked against CBASE_MEMORY (or the 128M estimate);
#   - the service built, sent as an S3 artifact and started over SSM send-command (no SSH), the port
#     checked free on the VM before the first start;
#   - THE VM ORIGIN HOOK: STACK_PARAMS (stack.sh) set to the stack's two parameters,
#       "ParameterKey=VmOrigin,ParameterValue=<the instance's public DNS name>"
#       "ParameterKey=VmPort,ParameterValue=<the port the app claimed>"
#     which every vm and ws route's behaviour (and CBASE_API_HOST's distribution) points at.
#     The static part (static.sh, stack.sh, cert.sh) is the same for a VM app as for a static site.
#   - group-init vm|static GROUP: one CloudFormation stack a group (the instance, or the bucket with
#     its tags), and the state bucket cbase-state-<account> with the first group. The static
#     group's bucket policy is set there, once a bucket, on aws:SourceAccount: s3:GetObject and
#     s3:ListBucket (so a missing file is a 404, not a 403) for cloudfront.amazonaws.com. Deploy
#     never edits it. A shared bucket holds public content only.

# vm_deploy: the VM side. Phase 3; it stops the deploy before anything is changed.
vm_deploy() {
  echo "cbase: $CONF_APP has a VM (CBASE_VM = $CONF_VM), and deploying the VM side (the service over SSM, its port, the VM origin of its routes) is phase 3, not built yet. A static site (CBASE_VM = none) deploys today. Nothing was changed." >&2
  exit 2
}
