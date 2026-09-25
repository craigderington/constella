#!/usr/bin/env bash
# Finish the catasterism.xyz static deploy once .xyz delegation is live.
#
# Everything before this is already done: the bucket exists and holds the page,
# the ACM request exists, and its DNS validation records are published in the
# zone. This script waits for the cert, then builds the CDN in front of it.
#
# Safe to re-run: every step is idempotent or checked first.

set -euo pipefail

ZONE_ID="Z07364351Y9Z8HB4HY25A"
BUCKET="catasterism-xyz-site"
DOMAIN="catasterism.xyz"
CERT_ARN="arn:aws:acm:us-east-1:525424800022:certificate/41032c4c-57c5-4715-93c2-7f87065fc282"
ACCOUNT="525424800022"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }

say "1/5  Checking delegation"
if ! dig +short "$DOMAIN" NS @8.8.8.8 | grep -q awsdns; then
  echo "  $DOMAIN still has no public NS records."
  echo "  The registry has not published delegation yet — finish registrant"
  echo "  verification and wait for propagation, then re-run this script."
  exit 1
fi
echo "  delegation live"

say "2/5  Waiting for the certificate (ACM validates from the records already in the zone)"
aws acm wait certificate-validated --certificate-arn "$CERT_ARN" --region us-east-1
echo "  issued"

say "3/5  CloudFront distribution"
EXISTING=$(aws cloudfront list-distributions \
  --query "DistributionList.Items[?Aliases.Items && contains(Aliases.Items, '$DOMAIN')].Id" \
  --output text 2>/dev/null || true)

if [ -n "$EXISTING" ] && [ "$EXISTING" != "None" ]; then
  DIST_ID="$EXISTING"
  echo "  reusing $DIST_ID"
else
  OAC_ID=$(aws cloudfront create-origin-access-control --origin-access-control-config \
    "Name=catasterism-oac,Description=catasterism site,SigningProtocol=sigv4,SigningBehavior=always,OriginAccessControlOriginType=s3" \
    --query 'OriginAccessControl.Id' --output text 2>/dev/null \
    || aws cloudfront list-origin-access-controls \
         --query "OriginAccessControlList.Items[?Name=='catasterism-oac'].Id" --output text)

  cat > /tmp/cf-catasterism.json <<JSON
{
  "CallerReference": "catasterism-$(date +%s)",
  "Aliases": { "Quantity": 2, "Items": ["$DOMAIN", "www.$DOMAIN"] },
  "DefaultRootObject": "index.html",
  "Origins": { "Quantity": 1, "Items": [{
    "Id": "s3-$BUCKET",
    "DomainName": "$BUCKET.s3.us-east-1.amazonaws.com",
    "OriginAccessControlId": "$OAC_ID",
    "S3OriginConfig": { "OriginAccessIdentity": "" }
  }]},
  "DefaultCacheBehavior": {
    "TargetOriginId": "s3-$BUCKET",
    "ViewerProtocolPolicy": "redirect-to-https",
    "AllowedMethods": { "Quantity": 2, "Items": ["GET","HEAD"] },
    "Compress": true,
    "CachePolicyId": "658327ea-f89d-4fab-a63d-7e88639e58f6"
  },
  "Comment": "catasterism.xyz project site",
  "Enabled": true,
  "ViewerCertificate": {
    "ACMCertificateArn": "$CERT_ARN",
    "SSLSupportMethod": "sni-only",
    "MinimumProtocolVersion": "TLSv1.2_2021"
  },
  "HttpVersion": "http2and3"
}
JSON
  DIST_ID=$(aws cloudfront create-distribution --distribution-config file:///tmp/cf-catasterism.json \
    --query 'Distribution.Id' --output text)
  echo "  created $DIST_ID"
fi

DIST_DOMAIN=$(aws cloudfront get-distribution --id "$DIST_ID" --query 'Distribution.DomainName' --output text)

say "4/5  Letting CloudFront read the bucket"
cat > /tmp/bucket-policy.json <<JSON
{ "Version": "2012-10-17", "Statement": [{
  "Sid": "AllowCloudFrontRead",
  "Effect": "Allow",
  "Principal": { "Service": "cloudfront.amazonaws.com" },
  "Action": "s3:GetObject",
  "Resource": "arn:aws:s3:::$BUCKET/*",
  "Condition": { "StringEquals": {
    "AWS:SourceArn": "arn:aws:cloudfront::$ACCOUNT:distribution/$DIST_ID" }}
}]}
JSON
aws s3api put-bucket-policy --bucket "$BUCKET" --policy file:///tmp/bucket-policy.json
echo "  policy applied"

say "5/5  Pointing the domain at it"
cat > /tmp/alias.json <<JSON
{ "Comment": "catasterism.xyz -> CloudFront", "Changes": [
  { "Action": "UPSERT", "ResourceRecordSet": { "Name": "$DOMAIN.", "Type": "A",
      "AliasTarget": { "HostedZoneId": "Z2FDTNDATAQYW2", "DNSName": "$DIST_DOMAIN.", "EvaluateTargetHealth": false }}},
  { "Action": "UPSERT", "ResourceRecordSet": { "Name": "$DOMAIN.", "Type": "AAAA",
      "AliasTarget": { "HostedZoneId": "Z2FDTNDATAQYW2", "DNSName": "$DIST_DOMAIN.", "EvaluateTargetHealth": false }}},
  { "Action": "UPSERT", "ResourceRecordSet": { "Name": "www.$DOMAIN.", "Type": "A",
      "AliasTarget": { "HostedZoneId": "Z2FDTNDATAQYW2", "DNSName": "$DIST_DOMAIN.", "EvaluateTargetHealth": false }}}
]}
JSON
aws route53 change-resource-record-sets --hosted-zone-id "$ZONE_ID" \
  --change-batch file:///tmp/alias.json --query 'ChangeInfo.Status' --output text

say "Done — https://$DOMAIN"
echo "CloudFront takes 5-15 minutes to finish deploying the first time."
echo
echo "To publish a change later:"
echo "  aws s3 cp site/index.html s3://$BUCKET/index.html \\"
echo "    --content-type 'text/html; charset=utf-8' --cache-control 'public,max-age=300'"
echo "  aws cloudfront create-invalidation --distribution-id $DIST_ID --paths '/*'"
