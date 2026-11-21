param([Parameter(Mandatory)][string]$SdkTracePath,[switch]$AllCandidates)
$ErrorActionPreference='Stop'
# Read only the already-attempted public endpoint, never its session query,
# tokens or authentication headers. This probe performs TLS only, no HTTP/API.
$stream=[IO.File]::Open((Get-Item -LiteralPath $SdkTracePath).FullName,'Open','Read','ReadWrite')
$reader=[IO.StreamReader]::new($stream)
$endpoint=$null
$candidates=[Collections.Generic.List[Uri]]::new()
try {
    while(($line=$reader.ReadLine()) -ne $null) {
        $attempt=$line -match '^\[\+\d+ms\] control endpoint=(wss://[^\s?]+)'
        if(!$attempt -and $AllCandidates) {
            $candidateLine=$line -match '^\[\+\d+ms\] control candidate(?: selected)? sig=\S+ ctrl=(wss://[^\s?]+)'
        } else {$candidateLine=$false}
        if($attempt -or $candidateLine) {
            $candidate=[Uri]$Matches[1]
            if($candidate.Scheme -ceq 'wss' -and !$candidate.UserInfo -and
               !$candidate.Query -and !$candidate.Fragment -and $candidate.Port -eq 5443) {
                if($attempt) {$endpoint=$candidate}
                if($candidates.Count -lt 4 -and !$candidates.Contains($candidate)) {$candidates.Add($candidate)}
            }
        }
    }
} finally {$reader.Dispose()}
if(!$endpoint) {throw 'No token-free previously attempted GCC endpoint found'}
if(!('YuanshiControlTlsProbeV1' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography.X509Certificates;
public sealed class YuanshiControlTlsProbeV1 {
    public bool TcpConnected;
    public bool TlsValidated;
    public string PolicyErrors = "NotObserved";
    public string ErrorCategory = "None";
    public string LeafNotBeforeUtc;
    public string LeafNotAfterUtc;
    public string[] ChainStatuses = new string[0];
    public string[] ChainValidity = new string[0];
    public static YuanshiControlTlsProbeV1 Run(string host, int port) {
        var result=new YuanshiControlTlsProbeV1();
        using(var tcp=new TcpClient()) {
            try {
                if(!tcp.ConnectAsync(host,port).Wait(5000)) {
                    result.ErrorCategory="TcpTimeout";return result;
                }
                result.TcpConnected=true;
                using(var ssl=new SslStream(tcp.GetStream(),false,(sender,cert,chain,errors)=> {
                    result.PolicyErrors=errors.ToString();
                    if(cert!=null) {
                        var leaf=new X509Certificate2(cert);
                        result.LeafNotBeforeUtc=leaf.NotBefore.ToUniversalTime().ToString("O");
                        result.LeafNotAfterUtc=leaf.NotAfter.ToUniversalTime().ToString("O");
                        leaf.Dispose();
                    }
                    if(chain!=null) {
                        var statuses=new List<string>();
                        var validity=new List<string>();
                        foreach(var status in chain.ChainStatus) statuses.Add(status.Status.ToString());
                        foreach(var element in chain.ChainElements) validity.Add(
                            element.Certificate.NotBefore.ToUniversalTime().ToString("O")+" / "+
                            element.Certificate.NotAfter.ToUniversalTime().ToString("O"));
                        result.ChainStatuses=statuses.ToArray();
                        result.ChainValidity=validity.ToArray();
                    }
                    // Never accept an invalid certificate, even for diagnostics.
                    return errors==SslPolicyErrors.None;
                })) {
                    if(!ssl.AuthenticateAsClientAsync(host).Wait(5000)) {
                        result.ErrorCategory="TlsTimeout";return result;
                    }
                    result.TlsValidated=ssl.IsAuthenticated;
                }
            } catch(Exception ex) {
                while(ex.InnerException!=null) ex=ex.InnerException;
                // Exception messages may contain endpoint data. Type only.
                result.ErrorCategory=ex.GetType().Name;
            }
        }
        return result;
    }
}
'@
}
$targets=if($AllCandidates){$candidates.ToArray()}else{@($endpoint)}
$index=0
foreach($target in $targets) {
    [ordered]@{CandidateIndex=$index;PreviouslyAttempted=($target -eq $endpoint);
        Tls=[YuanshiControlTlsProbeV1]::Run($target.DnsSafeHost,$target.Port)}|ConvertTo-Json -Depth 4
    $index++
}
