# changeset_replacements.jq - the resources a CloudFormation change set may
# recreate, space-separated ("" for none). deploy.sh stops on any of them.
#
# Input: `aws cloudformation describe-change-set --output json`.
# A resource counts when its Replacement is "True", or "Conditional" with a
# recreating detail that is not harmless. A detail is harmless when its cause
# is an attribute of another resource (ChangeSource ResourceAttribute,
# CausingEntity "<Resource>.<Attr>") that is itself not replaced: CloudFormation
# says "Conditional" for FunctionUrl on Function.Arn whenever Function is
# modified, but an attribute of a resource that is only modified keeps its value.
[.Changes[].ResourceChange | select(.Replacement == "True") | .LogicalResourceId] as $replaced
| def harmless:
    .ChangeSource == "ResourceAttribute"
    and (((.CausingEntity // "") | split(".")[0]) as $r | ($replaced | index($r)) == null);
[.Changes[].ResourceChange
  | select(.Replacement == "True"
      or (.Replacement == "Conditional"
          and ([.Details[] | select(.Target.RequiresRecreation != "Never") | select(harmless | not)] | length > 0)))
  | .LogicalResourceId]
| join(" ")
