# shellcheck shell=bash
# group.sh - a group's own CloudFormation stack: group-init and group-destroy (README.md "./cbase
# deploy tool"). Sourced by tools/cbase.
#
# GROUP-INIT KIND GROUP is the ONE command that makes a VM or a group's bucket; deploy never does.
# One stack per group, in the CLI's region (AWS_REGION, or the profile's):
#   cbase-group-vm-<group>      an EC2 instance (default t4g.nano, arm64, on the newest Amazon Linux
#                               2023 the SSM public parameter names when the group is first made;
#                               --type picks another arm64 type), tagged cbase:vm-group=<group>, in
#                               the group's own VPC (an Amazon IPv6 /56, one subnet in a zone that
#                               offers the type, an internet gateway). IPv6-only by default: no
#                               public IPv4 and no Elastic IP; the boot (vm_boot.sh, the user-data)
#                               claims the subnet's ::10, a fixed address that survives a
#                               replacement (the AAAA record of the origin names it), and points
#                               the SSM agent at the dual-stack endpoints. --ipv4 makes the subnet
#                               dual-stack with an Elastic IP (a public DNS name that survives a
#                               stop, the origin, addr=dualstack), about $3.60 a month more. An
#                               instance profile with SSM's managed-instance policy, s3:GetObject on
#                               s3://cbase-state-<account>/artifacts/*, ssm:GetParametersByPath on
#                               /cbase/* and ec2:AssignIpv6Addresses in its subnet; IMDSv2 required
#                               (over IPv6 as well); an encrypted 8 GB gp3 disk; and a security
#                               group open on 8700-8799, the apps' ports, to CloudFront's IPv6
#                               origin-facing managed prefix list alone (with --ipv4, a second to
#                               its IPv4 one; no SSH, no reverse proxy: vm.sh says why);
#   cbase-group-static-<group>  the bucket (versioned, noncurrent versions kept 30 days, public
#                               access blocked, owner-enforced, SSE-S3), tagged
#                               cbase:static-group=<group>, kept when its stack goes (Retain), and
#                               its one bucket policy: s3:GetObject and s3:ListBucket (so a missing
#                               file is a 404, not a 403) for cloudfront.amazonaws.com on
#                               aws:SourceAccount, the account's own distributions. Deploy never
#                               edits it, and a shared bucket holds public content only.
# Once the change set is accepted, it makes the state bucket cbase-state-<account> when there is none (versioned, public
# access blocked; an existing one of the account's is used as it is), and the group's empty state
# file (If-None-Match: *, so one that exists is left alone).
#
# A VM costs money, so a VM group's change set is printed with its cost class and runs only with
# --yes or a "yes" typed at the terminal. A re-run is an update through the same change set and
# the same guards as deploy's (stack.sh): drift stops it, and a change set that would replace or
# remove the bucket, or the instance (every member's data is on its disk), is refused. A re-run
# keeps the instance's image and, without --type, its type.
#
# GROUP-DESTROY KIND GROUP refuses while the group's state file names a member; for a VM group, also
# while the instance holds any app's /srv/cbase/<app>/data (undeploy keeps it) unless --delete-data.
# It asks as group-init does (--yes), deletes the group's stack and its state file. The bucket of a
# static group is kept with its files (Retain), and its group tag is taken off, so the name can be
# made again; deleting the bucket is left to the person (aws s3 rb).

GROUP_AMI_PARAM=/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64
GROUP_PREFIX_LIST=com.amazonaws.global.ipv6.cloudfront.origin-facing
GROUP_PREFIX_LIST_IPV4=com.amazonaws.global.cloudfront.origin-facing
GROUP_TYPE_DEFAULT=t4g.nano

group_stack() { printf 'cbase-group-%s-%s' "$1" "$2"; }

# group_cost TYPE: a sentence of what the instance costs a month (us-east-1 on-demand Linux, 730 h).
group_cost() {
  local m
  case "$1" in
    t4g.nano) m=3 ;; t4g.micro) m=6 ;; t4g.small) m=12 ;; t4g.medium) m=25 ;;
    t4g.large) m=49 ;; t4g.xlarge) m=98 ;; t4g.2xlarge) m=196 ;;
    *) printf 'a %s: see https://aws.amazon.com/ec2/pricing/on-demand/ for its price' "$1"; return 0 ;;
  esac
  printf 'a %s runs about \x24%s/month (us-east-1 on-demand), plus about \x240.64/month for its 8 GB disk' "$1" "$m"
}

# group_prefix_list NAME: the id of the managed prefix list NAME (CloudFront's origin-facing lists).
group_prefix_list() {
  local id
  id="$(aws_call ec2 describe-managed-prefix-lists --filters "Name=prefix-list-name,Values=$1" \
          --query 'PrefixLists[0].PrefixListId' --output text)" || aws_fail "finding $1"
  [[ "$id" =~ ^pl-[0-9a-f]+$ ]] || { echo "cbase: there is no $1 prefix list in $STACK_REGION" >&2; exit 1; }
  printf '%s' "$id"
}

# group_confirm WHAT: true with --yes (GROUP_YES=1), or a "yes" typed at a terminal; else false.
GROUP_YES=""
group_confirm() {
  local answer=""
  [ "$GROUP_YES" != 1 ] || return 0
  if [ -t 0 ]; then
    printf '%s? Type yes to go on: ' "$1"
    read -r answer || answer=""
    [ "$answer" = yes ] && return 0
  fi
  return 1
}

# group_state_bucket_found: status 0 when the account has its state bucket, 1 when it has none.
group_state_bucket_found() {
  local bucket
  bucket="$(state_bucket)"
  aws_call s3api head-bucket --bucket "$bucket" > /dev/null && return 0
  case "$(aws_code)" in 404|NoSuchBucket|NotFound) return 1 ;; *) aws_fail "reading the state bucket s3://$bucket" ;; esac
}

# group_state_bucket REGION: the state bucket, made when the account has none.
group_state_bucket() {
  local region="$1" bucket
  bucket="$(state_bucket)"
  if group_state_bucket_found; then
    echo "state bucket: s3://$bucket"
    return 0
  fi
  group_state_bucket_make "$region"
}

# group_state_bucket_adopted REGION GROUP: deploy's, for an adopted vm group (CBASE_VM_STACK),
# which no group-init made: the state bucket made when the account has none, as group-init makes
# it (private, versioned, public access blocked). A bucket is free and harmless; the VM is not, and
# only group-init makes one. The app's join then writes the group's state file with If-None-Match,
# a grandfathered member (CBASE_VM_PORT, CBASE_VM_DATA) recorded as it is. A dry run makes
# nothing: it says what a deploy would make, and goes on as if the bucket held no state file.
group_state_bucket_adopted() {
  local region="$1" group="$2" bucket
  bucket="$(state_bucket)"
  group_state_bucket_found && return 0
  if [ "${DRY_RUN:-}" = 1 ]; then
    echo "dry run: would create state bucket s3://$bucket ($region; versioned, public access blocked) and seed $(state_key vm "$group") with $CONF_APP${CONF_VM_PORT:+ (grandfathered: port $CONF_VM_PORT, data $CONF_VM_DATA)}"
    # shellcheck disable=SC2034 # state.sh reads it
    STATE_NO_BUCKET=1
    return 0
  fi
  group_state_bucket_make "$region"
}

# group_state_bucket_make REGION: the state bucket, made (a name this account holds already is it).
group_state_bucket_make() {
  local region="$1" bucket cfg=()
  bucket="$(state_bucket)"
  [ "$region" = us-east-1 ] || cfg=(--create-bucket-configuration "LocationConstraint=$region")
  if ! aws_call s3api create-bucket --bucket "$bucket" --region "$region" ${cfg[@]+"${cfg[@]}"} > /dev/null; then
    case "$(aws_code)" in
      BucketAlreadyOwnedByYou) ;;   # made since the read, by this account: it is the one
      BucketAlreadyExists) echo "cbase: the state bucket's name s3://$bucket is another account's" >&2; exit 1 ;;
      *) aws_fail "making the state bucket s3://$bucket" ;;
    esac
  fi
  aws_call s3api put-public-access-block --bucket "$bucket" --region "$region" \
    --public-access-block-configuration BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true \
    > /dev/null || aws_fail "blocking public access to s3://$bucket"
  aws_call s3api put-bucket-versioning --bucket "$bucket" --region "$region" --versioning-configuration Status=Enabled \
    > /dev/null || aws_fail "turning on versioning of s3://$bucket"
  echo "state bucket: s3://$bucket made ($region; versioned, public access blocked)"
}

# group_render_vm GROUP FILE [ipv4] / group_render_static GROUP FILE: the group's template. A vm
# group's is IPv6-only, or, with ipv4 (group-init --ipv4), dual-stack with a public IPv4.
group_render_vm() {
  local g="$1" v4="${3:-}"
  {
    cat << EOF
AWSTemplateFormatVersion: "2010-09-09"
# The vm group $g, made by ./cbase group-init (tools/cbase.d/group.sh). Edits by hand are drift.
Description: "cbase vm group $g: the instance its apps run on$([ -n "$v4" ] && echo ", dual-stack" || echo ", IPv6-only")"

Parameters:
  InstanceType:
    Type: String
    Description: an arm64 (Graviton) instance type; cbase builds its servers for linux arm64
  ImageId:
    Type: AWS::EC2::Image::Id
    Description: the Amazon Linux 2023 arm64 image the group was first made with ($GROUP_AMI_PARAM)
  AvailabilityZone:
    Type: AWS::EC2::AvailabilityZone::Name
    Description: the subnet's zone, one that offers the instance type (group-init picks it)
  CloudFrontPrefixList:
    Type: String
    Description: CloudFront's IPv6 origin-facing managed prefix list ($GROUP_PREFIX_LIST) in this region
    AllowedPattern: "pl-[0-9a-f]+"
EOF
    [ -z "$v4" ] || cat << EOF
  CloudFrontPrefixListIpv4:
    Type: String
    Description: CloudFront's IPv4 origin-facing managed prefix list ($GROUP_PREFIX_LIST_IPV4) in this region
    AllowedPattern: "pl-[0-9a-f]+"
EOF
    cat << EOF
  ArtifactBucket:
    Type: String
    Description: the state bucket, whose artifacts/ the instance reads its apps' servers from

Resources:
  # The group's own network: a VPC with an Amazon-provided IPv6 /56 and one subnet, an internet
  # gateway and a ::/0 route. $([ -n "$v4" ] && echo "The subnet is dual-stack (--ipv4), its IPv4 the Elastic IP's." || echo "The subnet is IPv6-only: no IPv4 address and no NAT, as SSM, S3, IMDS and
  # CloudFront all speak IPv6 (vm_boot.sh points the SSM agent at their dual-stack endpoints).")
  Vpc:
    Type: AWS::EC2::VPC
    Properties:
      CidrBlock: 10.0.0.0/24   # EC2 requires one; $([ -n "$v4" ] && echo "the subnet's" || echo "no subnet uses it")
      EnableDnsSupport: true
      EnableDnsHostnames: true
      Tags:
        - Key: Name
          Value: cbase-vm-$g
  VpcIpv6:
    Type: AWS::EC2::VPCCidrBlock
    Properties:
      VpcId: !Ref Vpc
      AmazonProvidedIpv6CidrBlock: true
  Igw:
    Type: AWS::EC2::InternetGateway
    Properties:
      Tags:
        - Key: Name
          Value: cbase-vm-$g
  IgwAttach:
    Type: AWS::EC2::VPCGatewayAttachment
    Properties:
      VpcId: !Ref Vpc
      InternetGatewayId: !Ref Igw
  Subnet:
    Type: AWS::EC2::Subnet
    DependsOn: VpcIpv6
    Properties:
      VpcId: !Ref Vpc
      AvailabilityZone: !Ref AvailabilityZone
EOF
    if [ -n "$v4" ]; then
      cat << EOF
      CidrBlock: 10.0.0.0/24
      MapPublicIpOnLaunch: false   # the Elastic IP is its public IPv4
EOF
    else
      cat << EOF
      Ipv6Native: true
      EnableDns64: false
EOF
    fi
    cat << EOF
      Ipv6CidrBlock: !Select [0, !Cidr [!Select [0, !GetAtt Vpc.Ipv6CidrBlocks], 1, 64]]
      AssignIpv6AddressOnCreation: true
      PrivateDnsNameOptionsOnLaunch:
        HostnameType: resource-name
        EnableResourceNameDnsAAAARecord: true
      Tags:
        - Key: Name
          Value: cbase-vm-$g
  RouteTable:
    Type: AWS::EC2::RouteTable
    Properties:
      VpcId: !Ref Vpc
      Tags:
        - Key: Name
          Value: cbase-vm-$g
  DefaultRoute6:
    Type: AWS::EC2::Route
    DependsOn: IgwAttach
    Properties:
      RouteTableId: !Ref RouteTable
      DestinationIpv6CidrBlock: ::/0
      GatewayId: !Ref Igw
EOF
    [ -z "$v4" ] || cat << EOF
  DefaultRoute4:
    Type: AWS::EC2::Route
    DependsOn: IgwAttach
    Properties:
      RouteTableId: !Ref RouteTable
      DestinationCidrBlock: 0.0.0.0/0
      GatewayId: !Ref Igw
EOF
    cat << EOF
  SubnetRoutes:
    Type: AWS::EC2::SubnetRouteTableAssociation
    Properties:
      SubnetId: !Ref Subnet
      RouteTableId: !Ref RouteTable

  Role:
    Type: AWS::IAM::Role
    Properties:
      Description: cbase vm group $g's instance
      AssumeRolePolicyDocument:
        Version: "2012-10-17"
        Statement:
          - Effect: Allow
            Principal:
              Service: ec2.amazonaws.com
            Action: sts:AssumeRole
      ManagedPolicyArns:
        - !Sub "arn:\${AWS::Partition}:iam::aws:policy/AmazonSSMManagedInstanceCore"
      Policies:
        - PolicyName: cbase-apps
          PolicyDocument:
            Version: "2012-10-17"
            Statement:
              - Sid: TheAppsServers
                Effect: Allow
                Action: s3:GetObject
                Resource: !Sub "arn:\${AWS::Partition}:s3:::\${ArtifactBucket}/artifacts/*"
              # The apps' backups (serve's CBASE_BACKUP_*): s3://<state bucket>/backups/, and no
              # other key of the bucket. One role serves every app of the group, and any process on
              # the instance can read its credentials from IMDS, so a per-app prefix in IAM would
              # not hold one app from another: the boundary between apps is their Unix users.
              - Sid: TheAppsBackups
                Effect: Allow
                Action: [s3:GetObject, s3:PutObject]
                Resource: !Sub "arn:\${AWS::Partition}:s3:::\${ArtifactBucket}/backups/*"
              - Sid: TheAppsBackupsList
                Effect: Allow
                Action: s3:ListBucket
                Resource: !Sub "arn:\${AWS::Partition}:s3:::\${ArtifactBucket}"
                Condition:
                  StringLike:
                    s3:prefix: "backups/*"
              - Sid: TheAppsSettings
                Effect: Allow
                Action: ssm:GetParametersByPath
                Resource: !Sub "arn:\${AWS::Partition}:ssm:\${AWS::Region}:\${AWS::AccountId}:parameter/cbase/*"
              # The boot's claim of the fixed address (vm_boot.sh), on an interface of this subnet alone.
              - Sid: TheFixedAddress
                Effect: Allow
                Action: ec2:AssignIpv6Addresses
                Resource: !Sub "arn:\${AWS::Partition}:ec2:\${AWS::Region}:\${AWS::AccountId}:network-interface/*"
                Condition:
                  ArnEquals:
                    ec2:Subnet: !Sub "arn:\${AWS::Partition}:ec2:\${AWS::Region}:\${AWS::AccountId}:subnet/\${Subnet}"

  Profile:
    Type: AWS::IAM::InstanceProfile
    Properties:
      Roles: [!Ref Role]

  SecurityGroup:
    Type: AWS::EC2::SecurityGroup
    Properties:
      GroupDescription: cbase vm group $g - the app ports 8700-8799 from CloudFront (IPv6) alone
      VpcId: !Ref Vpc
      SecurityGroupIngress:
        - IpProtocol: tcp
          FromPort: 8700
          ToPort: 8799
          SourcePrefixListId: !Ref CloudFrontPrefixList
          Description: CloudFront IPv6 origin-facing addresses to the app ports
      SecurityGroupEgress:
        - IpProtocol: "-1"
          CidrIpv6: ::/0
          Description: all egress (IPv6)
EOF
    # The IPv4 list in a group of its own: a prefix list counts its maximum entries against the
    # group's rule quota (60), and CloudFront's two lists together pass it.
    [ -z "$v4" ] || cat << EOF
        - IpProtocol: "-1"
          CidrIp: 0.0.0.0/0
          Description: all egress (IPv4)
  SecurityGroupIpv4:
    Type: AWS::EC2::SecurityGroup
    Properties:
      GroupDescription: cbase vm group $g - the app ports 8700-8799 from CloudFront (IPv4) alone
      VpcId: !Ref Vpc
      SecurityGroupIngress:
        - IpProtocol: tcp
          FromPort: 8700
          ToPort: 8799
          SourcePrefixListId: !Ref CloudFrontPrefixListIpv4
          Description: CloudFront IPv4 origin-facing addresses to the app ports
EOF
    cat << EOF

  LaunchTemplate:
    Type: AWS::EC2::LaunchTemplate
    Properties:
      LaunchTemplateData:
        MetadataOptions:
          HttpTokens: required
          HttpEndpoint: enabled
          HttpProtocolIpv6: enabled
          HttpPutResponseHopLimit: 1
        BlockDeviceMappings:
          - DeviceName: /dev/xvda
            Ebs:
              VolumeSize: 8
              VolumeType: gp3
              Encrypted: true
              DeleteOnTermination: true

  Instance:
    Type: AWS::EC2::Instance
    DependsOn: [DefaultRoute6, $([ -n "$v4" ] && echo "DefaultRoute4, ")SubnetRoutes]
    Properties:
      ImageId: !Ref ImageId
      InstanceType: !Ref InstanceType
      IamInstanceProfile: !Ref Profile
      NetworkInterfaces:
        - DeviceIndex: "0"
          SubnetId: !Ref Subnet
          GroupSet: [!GetAtt SecurityGroup.GroupId$([ -n "$v4" ] && echo ", !GetAtt SecurityGroupIpv4.GroupId")]
          # A random address of the subnet: the boot claims the fixed one (OriginIpv6) itself.
          Ipv6AddressCount: 1
          AssociatePublicIpAddress: false
          DeleteOnTermination: true
      LaunchTemplate:
        LaunchTemplateId: !Ref LaunchTemplate
        Version: !GetAtt LaunchTemplate.LatestVersionNumber
      UserData:
        Fn::Base64: !Sub
          - |
EOF
    # vm_boot.sh without its comments and blank lines (the shebang stays), its ${ written ${!
    # (Fn::Sub's literal), its address the one Fn::Sub names.
    # shellcheck disable=SC2016,SC2154 # the ${ are literal; tools/cbase sets here
    awk 'NR == 1 || (!/^[[:space:]]*#/ && !/^$/)' "$here/cbase.d/vm_boot.sh" \
      | sed -e 's/\${/${!/g' -e "s/^want='@ORIGIN_IPV6@'\$/want='\${OriginIpv6}'/" -e 's/^/            /'
    cat << EOF
          - OriginIpv6: !Select [0, !Split ["/", !Select [1, !Cidr [!Select [0, !GetAtt Subnet.Ipv6CidrBlocks], 2, 4]]]]
      Tags:
        - Key: cbase:vm-group
          Value: $g
        - Key: Name
          Value: cbase-vm-$g
EOF
    [ -z "$v4" ] || cat << EOF

  Address:
    Type: AWS::EC2::EIP
    DependsOn: IgwAttach
    Properties:
      Domain: vpc
      InstanceId: !Ref Instance
EOF
    cat << EOF

Outputs:
  InstanceId:
    Value: !Ref Instance
  OriginIpv6:
    Description: the fixed IPv6 address the instance claims at boot, the subnet's ::10 (its AAAA record's)
    Value: !Select [0, !Split ["/", !Select [1, !Cidr [!Select [0, !GetAtt Subnet.Ipv6CidrBlocks], 2, 4]]]]
EOF
  } > "$2"
}

group_render_static() {
  local g="$1"
  cat > "$2" << EOF
AWSTemplateFormatVersion: "2010-09-09"
# The static group $g, made by ./cbase group-init (tools/cbase.d/group.sh). Edits by hand are drift.
Description: "cbase static group $g: the bucket its apps' files are in, a folder each"

Resources:
  Bucket:
    Type: AWS::S3::Bucket
    DeletionPolicy: Retain
    UpdateReplacePolicy: Retain
    Properties:
      VersioningConfiguration:
        Status: Enabled
      LifecycleConfiguration:
        Rules:
          - Id: noncurrent-versions-30-days
            Status: Enabled
            NoncurrentVersionExpiration:
              NoncurrentDays: 30
      PublicAccessBlockConfiguration:
        BlockPublicAcls: true
        IgnorePublicAcls: true
        BlockPublicPolicy: true
        RestrictPublicBuckets: true
      OwnershipControls:
        Rules:
          - ObjectOwnership: BucketOwnerEnforced
      BucketEncryption:
        ServerSideEncryptionConfiguration:
          - ServerSideEncryptionByDefault:
              SSEAlgorithm: AES256
      Tags:
        - Key: cbase:static-group
          Value: $g

  BucketPolicy:
    Type: AWS::S3::BucketPolicy
    Properties:
      Bucket: !Ref Bucket
      PolicyDocument:
        Version: "2012-10-17"
        Statement:
          - Sid: TheAccountsDistributionsRead
            Effect: Allow
            Principal:
              Service: cloudfront.amazonaws.com
            Action: [s3:GetObject, s3:ListBucket]
            Resource:
              - !GetAtt Bucket.Arn
              - !Sub "\${Bucket.Arn}/*"
            Condition:
              StringEquals:
                aws:SourceAccount: !Ref AWS::AccountId

Outputs:
  BucketName:
    Value: !Ref Bucket
EOF
}

# group_args KIND ARGS...: the flags group-init and group-destroy take. GROUP_YES, GROUP_TYPE,
# GROUP_IPV4, GROUP_DELETE_DATA.
GROUP_TYPE=""
GROUP_IPV4=""
GROUP_DELETE_DATA=""
group_args() {
  local cmd="$1" kind="$2"; shift 2
  while [ $# -gt 0 ]; do
    case "$1" in
      --yes) GROUP_YES=1 ;;
      --type)
        [ "$cmd" = group-init ] || { echo "cbase: $cmd takes no --type" >&2; exit 2; }
        [ $# -ge 2 ] || { echo "cbase: --type wants an instance type (t4g.small)" >&2; exit 2; }
        [[ "$2" =~ ^[a-z][a-z0-9-]*\.[a-z0-9]+$ ]] || { echo "cbase: $2 is not an instance type (t4g.small)" >&2; exit 2; }
        GROUP_TYPE="$2"; shift ;;
      --ipv4)
        if [ "$cmd" != group-init ] || [ "$kind" != vm ]; then echo "cbase: --ipv4 is group-init vm's alone" >&2; exit 2; fi
        GROUP_IPV4=1 ;;
      --delete-data)
        [ "$cmd" = group-destroy ] || { echo "cbase: $cmd takes no --delete-data" >&2; exit 2; }
        GROUP_DELETE_DATA=1 ;;
      *) echo "cbase: $cmd takes --yes$([ "$cmd" = group-init ] && echo ", --type TYPE and --ipv4" || echo " and --delete-data"), not $1" >&2; exit 2 ;;
    esac
    shift
  done
}

# group_init KIND GROUP
group_init() {
  local kind="$1" group="$2" region text archs mib image="" prefix zone
  region="$(aws_region_required)"
  # shellcheck disable=SC2034 # stack.sh reads STACK_REGION, STACK_TAGS and STACK_CAPS
  STACK_REGION="$region"
  STACK_NAME="$(group_stack "$kind" "$group")"
  STACK_TEMPLATE="$CB_TMP/group.yaml"
  # shellcheck disable=SC2034 # stack.sh reads it
  STACK_TAGS=("Key=cbase:group,Value=$kind/$group")
  STACK_CAPS=()
  STACK_PARAMS=()
  echo "group-init: $kind group $group in account $AWS_ACCOUNT ($region), stack $STACK_NAME"
  stack_status
  if [ "$kind" = vm ]; then
    if [ -z "$GROUP_TYPE" ] && [ "$STACK_STATUS" = NONE ]; then GROUP_TYPE="$GROUP_TYPE_DEFAULT"; fi
    if [ -n "$GROUP_TYPE" ]; then
      # shellcheck disable=SC2016 # the backticks are a JMESPath literal
      if ! text="$(aws_call ec2 describe-instance-types --instance-types "$GROUP_TYPE" \
                     --query 'InstanceTypes[0].[MemoryInfo.SizeInMiB,join(`,`,ProcessorInfo.SupportedArchitectures)]' --output text)"; then
        [ "$(aws_code)" != InvalidInstanceType ] || { echo "cbase: $GROUP_TYPE is not an EC2 instance type in $region" >&2; exit 1; }
        aws_fail "reading instance type $GROUP_TYPE"
      fi
      IFS=$'\t' read -r mib archs <<< "$text"
      [[ ",$archs," == *,arm64,* ]] || { echo "cbase: $GROUP_TYPE is $archs, and cbase builds the server for linux arm64 alone (make serve-binary): pick an arm64 (Graviton) type, t4g.nano to t4g.2xlarge" >&2; exit 1; }
      echo "instance: $(group_cost "$GROUP_TYPE"); $mib MiB, of which $((mib - VM_OS_RESERVE)) for apps ($VM_OS_RESERVE kept for the OS)"
      STACK_PARAMS+=("ParameterKey=InstanceType,ParameterValue=$GROUP_TYPE")
    else
      STACK_PARAMS+=("ParameterKey=InstanceType,UsePreviousValue=true")
    fi
    if [ "$STACK_STATUS" = NONE ]; then
      image="$(aws_call ssm get-parameter --name "$GROUP_AMI_PARAM" --query Parameter.Value --output text)" \
        || aws_fail "reading the Amazon Linux 2023 image ($GROUP_AMI_PARAM)"
      [[ "$image" =~ ^ami-[0-9a-f]+$ ]] || { echo "cbase: $GROUP_AMI_PARAM names \"${image:0:40}\", not an image" >&2; exit 1; }
      echo "image: $image (Amazon Linux 2023 arm64, kept for the group's life)"
      STACK_PARAMS+=("ParameterKey=ImageId,ParameterValue=$image")
    else
      STACK_PARAMS+=("ParameterKey=ImageId,UsePreviousValue=true")
    fi
    # The subnet's zone, one that offers the type, picked once: a new zone is a new subnet and instance.
    if [ "$STACK_STATUS" = NONE ]; then
      zone="$(aws_call ec2 describe-instance-type-offerings --location-type availability-zone \
                --filters "Name=instance-type,Values=$GROUP_TYPE" --query 'sort(InstanceTypeOfferings[].Location)[0]' --output text)" \
        || aws_fail "finding a zone that offers $GROUP_TYPE"
      [[ "$zone" =~ ^[a-z0-9-]+[a-z]$ ]] || { echo "cbase: no zone of $region offers $GROUP_TYPE" >&2; exit 1; }
      echo "zone: $zone (offers $GROUP_TYPE; kept for the group's life)"
      STACK_PARAMS+=("ParameterKey=AvailabilityZone,ParameterValue=$zone")
    else
      STACK_PARAMS+=("ParameterKey=AvailabilityZone,UsePreviousValue=true")
    fi
    prefix="$(group_prefix_list "$GROUP_PREFIX_LIST")"
    STACK_PARAMS+=("ParameterKey=CloudFrontPrefixList,ParameterValue=$prefix")
    if [ "$GROUP_IPV4" = 1 ]; then
      prefix="$(group_prefix_list "$GROUP_PREFIX_LIST_IPV4")"
      STACK_PARAMS+=("ParameterKey=CloudFrontPrefixListIpv4,ParameterValue=$prefix")
      echo "address: dual-stack (--ipv4): a public IPv4 is about \$3.60 a month, an Elastic IP"
    else
      echo "address: IPv6-only, no public IPv4 (--ipv4 adds one, about \$3.60 a month)"
    fi
    STACK_PARAMS+=("ParameterKey=ArtifactBucket,ParameterValue=$(state_bucket)")
    # shellcheck disable=SC2034 # stack.sh reads it
    STACK_CAPS=(CAPABILITY_IAM)
    group_render_vm "$group" "$STACK_TEMPLATE" ${GROUP_IPV4:+ipv4}
  else
    echo "bucket: S3 bills what it stores and serves (cents a month for a site)"
    group_render_static "$group" "$STACK_TEMPLATE"
  fi
  stack_changeset
  if [ -n "$STACK_CHANGES" ] && [ "$kind" = vm ]; then
    if ! group_confirm "Run this change set on $STACK_NAME"; then
      stack_discard
      echo "cbase: a VM costs money, so group-init vm runs its change set only with --yes (or yes typed at a terminal): the change set is deleted, and nothing was made or changed" >&2
      exit 2
    fi
  fi
  # The state bucket first: the instance's role names it, and the state file goes in it.
  group_state_bucket "$region"
  [ -z "$STACK_CHANGES" ] || stack_execute
  # The group's state file, empty, so apps and doctor see the group; one that exists is kept. A
  # --ipv4 group's records its origin: the instance's public DNS name (its Elastic IP's), dualstack.
  # An IPv6-only instance has no public DNS name of AWS's: CloudFront reaches it at an AAAA record
  # of its fixed address, which the group's first app names (CBASE_VM_ORIGIN) and its join records
  # (addr=ipv6, state.sh).
  local bucket key origin="" addr6
  bucket="$(state_bucket)"
  key="$(state_key "$kind" "$group")"
  if [ "$kind" = vm ] && [ "$GROUP_IPV4" != 1 ]; then
    addr6="$(stack_output OriginIpv6)"
    echo "origin: the instance claims $addr6 at boot, and keeps it across a replacement. Make an AAAA record for it (vm.example.com AAAA $addr6) and name it once in the group's first app's cbase.conf: CBASE_VM_ORIGIN = vm.example.com"
  elif [ "$kind" = vm ]; then
    origin="$(aws_call ec2 describe-instances --instance-ids "$(aws_group_find vm "$group")" \
                --query "Reservations[0].Instances[0].PublicDnsName" --output text)" || aws_fail "reading vm group $group's instance"
    [ "$origin" != None ] || origin=""
  fi
  printf 'cbase-state %s %s %s%s\n' "$STATE_VERSION" "$kind" "$group" "${origin:+ origin=$origin addr=dualstack}" > "$CB_TMP/state-new"
  local rc=0
  aws_s3_put "$bucket" "$key" "$CB_TMP/state-new" "*" || rc=$?
  case "$rc" in
    0) echo "state file: s3://$bucket/$key (no members yet)" ;;
    3) echo "state file: s3://$bucket/$key (kept as it is)" ;;
    *) exit "$rc" ;;
  esac
  echo "$kind group $group: $(aws_group_find "$kind" "$group")"
}

# group_destroy KIND GROUP
group_destroy() {
  local kind="$1" group="$2" region bucket key line data=() id=""
  region="$(aws_region_required)"
  # shellcheck disable=SC2034 # stack.sh reads it
  STACK_REGION="$region"
  STACK_NAME="$(group_stack "$kind" "$group")"
  echo "group-destroy: $kind group $group in account $AWS_ACCOUNT ($region), stack $STACK_NAME"
  state_read "$kind" "$group"
  if [ "${#ST_APPS[@]}" -gt 0 ]; then
    echo "cbase: $kind group $group still has $(printf '%s' "${#ST_APPS[@]}") member$([ "${#ST_APPS[@]}" = 1 ] || echo s): ${ST_APPS[*]}. Undeploy each first (./cbase undeploy in its repository); nothing was changed." >&2
    exit 1
  fi
  stack_status
  if [ "$STACK_STATUS" != NONE ]; then
    id="$(aws_group_find "$kind" "$group")"
    if [ "$kind" = vm ]; then
      VM_ID="$id"
      # The data undeploy keeps: the instance's disk goes with it.
      cat > "$CB_TMP/vm-data.sh" << 'EOF'
#!/bin/bash
set -euo pipefail
R="${CBASE_VM_ROOT:-}"
for d in "$R"/srv/cbase/*/data; do
  [ -d "$d" ] || continue
  a="${d%/data}"
  echo "CBASE: data ${a##*/}"
done
EOF
      vm_ssm_run "$CB_TMP/vm-data.sh" "data check of vm group $group"
      [ "$VM_SSM_STATUS" = Success ] || { echo "cbase: the data check on $VM_ID ended $VM_SSM_STATUS, so whether the instance holds data is not known: nothing was changed (--delete-data destroys it anyway)" >&2; [ "$GROUP_DELETE_DATA" = 1 ] || exit 1; }
      while IFS= read -r line; do data+=("${line#CBASE: data }"); done < <(grep '^CBASE: data ' "$CB_TMP/ssm-lines" || true)
    fi
  fi
  if [ "${#data[@]}" -gt 0 ] && [ "$GROUP_DELETE_DATA" != 1 ]; then
    echo "cbase: vm group $group's instance $VM_ID holds the data of ${data[*]} (/srv/cbase/<app>/data, kept by undeploy), which goes with the instance: refused. Copy it off, or run group-destroy --delete-data to destroy it. Nothing was changed." >&2
    exit 1
  fi
  if ! group_confirm "Delete $STACK_NAME$([ "$kind" = vm ] && echo ", and the instance with its disk")"; then
    echo "cbase: group-destroy deletes the group's stack only with --yes (or yes typed at a terminal): nothing was changed" >&2
    exit 2
  fi
  stack_delete
  if [ "$kind" = static ] && [ -n "$id" ]; then
    # Retain kept the bucket and its files; with its tag gone, a group-init of the name makes a new one.
    aws_call s3api delete-bucket-tagging --bucket "$id" > /dev/null || aws_fail "taking the group tag off s3://$id"
    echo "bucket: s3://$id is kept with its files, its cbase:static-group tag taken off (delete it with aws s3 rb s3://$id --force)"
  fi
  bucket="$(state_bucket)"
  key="$(state_key "$kind" "$group")"
  if [ "$STATE_ETAG" != "*" ]; then
    aws_call s3api delete-object --bucket "$bucket" --key "$key" > /dev/null || aws_fail "deleting s3://$bucket/$key"
    echo "state file: s3://$bucket/$key deleted"
  fi
  echo "$kind group $group is destroyed"
}
